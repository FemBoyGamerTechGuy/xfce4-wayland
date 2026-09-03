/* xwc-icon.c — XDG icon theme lookup, PNG/XPM decode, rescale, cache.
 *
 * The icon theme spec (freedesktop) is followed in its common path:
 * search <data dirs>/icons/<theme>/... for "<name>.<ext>" with .png and
 * .xpm renderable; pick the file whose size is closest >= the request;
 * /usr/share/pixmaps is the flat legacy fallback. Theme selection
 * follows the desktop's own configuration:
 *   $XW_ICON_THEME (panel.conf icon_theme) > gtk-3.0/settings.ini
 *   gtk-icon-theme-name > gtk-4.0 > XFCE xfconf xsettings IconThemeName
 *   > "hicolor" (the guaranteed fallback). Theme inheritance
 *   (Inherit= in index.theme) is followed depth-limited, cycle-safe;
 *   hicolor terminates every chain.
 * Icon= values carrying an extension ("foo.png") are looked up both
 * verbatim and stripped. Misses are logged once per name@size and
 * fall back to the caller's generic glyph.
 * SVG is not renderable without a vector stack and is skipped
 * (documented deviation). PNG needs the optional libpng
 * (XW_PNG=auto|1|0); XPM is parsed in-house.
 *
 * Everything is cached: one on-disk index (built on first use) and one
 * decoded-surface cache keyed by "name@size". Single-threaded clients.
 */
#include "xwc.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef XW_HAVE_PNG
#include <png.h>
#endif

/* ---------------------------------------------------------------- index */

/* One candidate file found under an icon root. The index is sorted by
 * name so lookups binary-search; same-named entries (several sizes)
 * stay adjacent. */
struct icon_entry {
    char *name;  /* basename without extension */
    char *path;  /* absolute or $-relative path of the file */
    int size;    /* icon size from the directory layout (48x48 -> 48);
                    0 = unknown, detected after decode */
};

#define ICON_INDEX_MAX 24000
static struct icon_entry g_index[ICON_INDEX_MAX];
static int g_index_n;
static bool g_index_built;

/* parsed result of one lookup (not owned) */
struct icon_file {
    const char *path;
    int size;
};

/* -------------------------------------------------------------- cache */

struct icon_cache {
    char *key; /* "name@size" */
    struct xwc_icon icon;
};

static struct icon_cache *g_cache;
static int g_cache_n, g_cache_cap;

static void cache_put(const char *key, uint32_t *pix, int w, int h) {
    if (g_cache_n == g_cache_cap) {
        int cap = g_cache_cap ? g_cache_cap * 2 : 64;
        struct icon_cache *nc = realloc(g_cache, (size_t)cap * sizeof(*nc));
        if (!nc)
            return; /* cache is an optimization: drop on OOM */
        g_cache = nc;
        g_cache_cap = cap;
    }
    struct icon_cache *e = &g_cache[g_cache_n++];
    e->key = strdup(key);
    if (!e->key) {
        g_cache_n--;
        return;
    }
    e->icon.w = w;
    e->icon.h = h;
    e->icon.pix = pix;
}

static const struct xwc_icon *cache_get(const char *key) {
    for (int i = 0; i < g_cache_n; i++)
        if (strcmp(g_cache[i].key, key) == 0)
            return &g_cache[i].icon;
    return NULL;
}

/* one-shot miss log: a name@size that failed to resolve is logged
 * once so a menu redraw cannot spam (and the user can see WHY an icon
 * is missing) */
static char g_missed[64][600];
static int g_missed_n;

static void log_miss(const char *key, const char *name) {
    for (int i = 0; i < g_missed_n; i++)
        if (strcmp(g_missed[i], key) == 0)
            return;
    if (g_missed_n >= 64)
        return;
    snprintf(g_missed[g_missed_n++], sizeof(g_missed[0]), "%s", key);
    fprintf(stderr, "xw: icon missing: %s (size key %s) — using fallback\n",
            name, key);
}

void xwc_icon_flush(void) {
    for (int i = 0; i < g_index_n; i++) {
        free(g_index[i].name);
        free(g_index[i].path);
    }
    g_index_n = 0;
    g_index_built = false;
    for (int i = 0; i < g_cache_n; i++) {
        free(g_cache[i].key);
        free(g_cache[i].icon.pix);
    }
    free(g_cache);
    g_cache = NULL;
    g_cache_n = g_cache_cap = 0;
    g_missed_n = 0;
}

bool xwc_icon_png(void) {
#ifdef XW_HAVE_PNG
    return true;
#else
    return false;
#endif
}

/* ------------------------------------------------------- index building */

static bool renderable_ext(const char *name, size_t len) {
    if (len < 4)
        return false;
    const char *ext = name + len - 4;
    if (ext[0] != '.')
        return false;
    if ((ext[1] == 'p' || ext[1] == 'P') &&
        (ext[2] == 'n' || ext[2] == 'N') &&
        (ext[3] == 'g' || ext[3] == 'G'))
        return true;
    if ((ext[1] == 'x' || ext[1] == 'X') &&
        (ext[2] == 'p' || ext[2] == 'P') &&
        (ext[3] == 'm' || ext[3] == 'M'))
        return true;
    return false;
}

/* "48x48" -> 48; "scalable"/"symbolic" -> 512 (vector dir: only PNGs
 * found there are usable, treat as huge); any other component -> 0 */
static int dir_size_of(const char *comp) {
    if (!comp || !*comp)
        return 0;
    if (strcmp(comp, "scalable") == 0 || strcmp(comp, "symbolic") == 0)
        return 512;
    char *end = NULL;
    long w = strtol(comp, &end, 10);
    if (end && *end == 'x') {
        char *end2 = NULL;
        long h = strtol(end + 1, &end2, 10);
        if (end2 && *end2 == 0 && w == h && w > 0 && w <= 1024)
            return (int)w;
    }
    return 0;
}

static void index_add(const char *path, const char *base, int size) {
    if (g_index_n >= ICON_INDEX_MAX)
        return;
    size_t len = strlen(base);
    if (!renderable_ext(base, len))
        return;
    struct icon_entry *e = &g_index[g_index_n];
    e->name = strndup(base, len - 4);
    e->path = strdup(path);
    if (!e->name || !e->path) {
        free(e->name);
        free(e->path);
        return;
    }
    e->size = size;
    g_index_n++;
}

/* depth-limited recursive walk of one theme directory */
static void walk_theme_dir(const char *dir, int size_hint, int depth) {
    if (depth > 5)
        return; /* themes are size/context level deep at most */
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/%.240s", dir, de->d_name) >=
            (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            int sz = dir_size_of(de->d_name);
            walk_theme_dir(path, sz ? sz : size_hint, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            index_add(path, de->d_name, size_hint);
        }
    }
    closedir(d);
}

/* parse Inherit= from <root>/<theme>/index.theme (comma-separated
 * parent theme names, the [Icon Theme] group) */
static void theme_parents(const char *icons_root, const char *theme,
                          char out[4][128], int *n_out) {
    *n_out = 0;
    if (!icons_root || !theme)
        return;
    char path[1024];
    if (snprintf(path, sizeof(path), "%.480s/%.240s/index.theme",
                 icons_root, theme) >= (int)sizeof(path))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    bool in_group = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            in_group = strncmp(line, "[Icon Theme]", 12) == 0;
            continue;
        }
        if (!in_group || strncmp(line, "Inherit", 7) != 0)
            continue;
        const char *v = strchr(line, '=');
        if (!v)
            continue;
        v++;
        char names[512];
        snprintf(names, sizeof(names), "%.500s", v);
        char *save = NULL;
        for (char *tok = strtok_r(names, ", \t\r\n", &save);
             tok && *n_out < 4; tok = strtok_r(NULL, ", \t\r\n", &save)) {
            if (*tok && strcmp(tok, theme) != 0 &&
                strcmp(tok, "hicolor") != 0)
                snprintf(out[(*n_out)++], 128, "%.120s", tok);
        }
        break;
    }
    fclose(f);
}

/* add a theme and its inheritance chain (depth-limited, cycle-safe;
 * hicolor always terminates a chain and is never re-walked) */
static void add_theme(const char *icons_root, const char *theme, int depth) {
    if (!icons_root || !*icons_root || !theme || !*theme)
        return;
    if (depth > 3)
        return; /* chains deeper than that are broken configs */
    if (strcmp(theme, "hicolor") == 0) {
        char path[1024];
        if (snprintf(path, sizeof(path), "%.480s/hicolor", icons_root) <
            (int)sizeof(path))
            walk_theme_dir(path, 0, 0);
        return;
    }
    char path[1024];
    if (snprintf(path, sizeof(path), "%.480s/%.240s", icons_root, theme) >=
        (int)sizeof(path))
        return;
    walk_theme_dir(path, 0, 0);
    /* Inherit= parents come after the theme itself */
    char parents[4][128];
    int n_parents = 0;
    theme_parents(icons_root, theme, parents, &n_parents);
    for (int i = 0; i < n_parents; i++)
        add_theme(icons_root, parents[i], depth + 1);
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const struct icon_entry *)a)->name,
                  ((const struct icon_entry *)b)->name);
}

/* the desktop's configured icon theme, from the places GTK/XFCE
 * actually store it. $XW_ICON_THEME (set from panel.conf) wins; then
 * gtk-3.0/gtk-4.0 settings.ini; then the XFCE xfconf xsettings xml. */
static void discover_theme(char *out, size_t n) {
    out[0] = 0;
    const char *env = getenv("XW_ICON_THEME");
    if (env && *env) {
        snprintf(out, n, "%.60s", env);
        return;
    }
    const char *xh = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char base[512];
    if (xh && *xh)
        snprintf(base, sizeof(base), "%.480s", xh);
    else if (home && *home)
        snprintf(base, sizeof(base), "%.480s/.config", home);
    else
        return;
    static const char *const gtk_ini[] = {"gtk-3.0/settings.ini",
                                          "gtk-4.0/settings.ini"};
    for (size_t i = 0; i < 2 && !out[0]; i++) {
        char path[600];
        snprintf(path, sizeof(path), "%.480s/%s", base, gtk_ini[i]);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *v = strstr(line, "gtk-icon-theme-name");
            if (!v)
                continue;
            char *eq = strchr(v, '=');
            if (!eq)
                continue;
            char val[192];
            snprintf(val, sizeof(val), "%.180s", eq + 1);
            char *nl = strpbrk(val, "\r\n");
            if (nl)
                *nl = 0;
            char *p = val;
            while (*p == ' ' || *p == '"')
                p++;
            char *end = p + strlen(p);
            while (end > p && (end[-1] == ' ' || end[-1] == '"'))
                *--end = 0;
            if (*p)
                snprintf(out, n, "%.60s", p);
            break;
        }
        fclose(f);
    }
    if (out[0])
        return;
    /* xfconf: <property name="IconThemeName" type="empty"><property
     * name="" type="string" value="Papirus"/></property> or the flat
     * <property name="IconThemeName" type="string" value="..."/> */
    char path[600];
    snprintf(path, sizeof(path),
             "%.480s/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml", base);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "IconThemeName") && strstr(line, "value=")) {
            char *v = strstr(line, "value=\"");
            if (v) {
                v += 7;
                char *end = strchr(v, '"');
                if (end && end > v) {
                    size_t len = (size_t)(end - v);
                    if (len < n) {
                        memcpy(out, v, len);
                        out[len] = 0;
                    }
                    break;
                }
            }
        }
    }
    fclose(f);
}

static void index_build(void) {
    g_index_built = true;

    char theme[128];
    discover_theme(theme, sizeof(theme));

    /* roots in priority order (user overrides system, matching the
     * icon theme spec's lookup order) */
    char roots[8][512];
    int n_roots = 0;

    const char *xh = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xh && *xh)
        snprintf(roots[n_roots++], sizeof(roots[0]), "%.480s/icons", xh);
    else if (home && *home)
        snprintf(roots[n_roots++], sizeof(roots[0]),
                 "%.240s/.local/share/icons", home);
    if (home && *home)
        snprintf(roots[n_roots++], sizeof(roots[0]), "%.240s/.icons", home);

    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs)
        dirs = "/usr/local/share:/usr/share";
    char dcopy[768];
    snprintf(dcopy, sizeof(dcopy), "%.700s", dirs);
    char *save = NULL;
    for (char *tok = strtok_r(dcopy, ":", &save);
         tok && n_roots < 6; tok = strtok_r(NULL, ":", &save))
        snprintf(roots[n_roots++], sizeof(roots[0]), "%.480s/icons", tok);
    /* the flat legacy directory */
    snprintf(roots[n_roots++], sizeof(roots[0]), "%s", "/usr/share/pixmaps");

    for (int i = 0; i < n_roots; i++) {
        if (strstr(roots[i], "pixmaps")) {
            /* flat: no theme layer, no size directory */
            DIR *d = opendir(roots[i]);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    if (de->d_name[0] == '.')
                        continue;
                    char path[1024];
                    if (snprintf(path, sizeof(path), "%s/%.240s", roots[i],
                                 de->d_name) >= (int)sizeof(path))
                        continue;
                    index_add(path, de->d_name, 0);
                }
                closedir(d);
            }
            continue;
        }
        /* the configured theme (with its Inherit= chain) first, then
         * hicolor as the spec's guaranteed fallback */
        if (theme[0])
            add_theme(roots[i], theme, 0);
        add_theme(roots[i], "hicolor", 0);
    }

    qsort(g_index, (size_t)g_index_n, sizeof(g_index[0]), entry_cmp);
}

/* best file for `name`: prefer size >= request ascending; else the
 * largest available. Returns false when nothing matched. */
static bool index_find(const char *name, int want, struct icon_file *out) {
    if (!g_index_built)
        index_build();
    /* binary search for the name run */
    int lo = 0, hi = g_index_n - 1, first = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(g_index[mid].name, name);
        if (c == 0) {
            first = mid;
            hi = mid - 1; /* keep descending to the first of the run */
        } else if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (first < 0)
        return false;

    const struct icon_entry *best = NULL;
    for (int i = first; i < g_index_n && strcmp(g_index[i].name, name) == 0;
         i++) {
        const struct icon_entry *e = &g_index[i];
        if (e->size <= 0) {
            /* size unknown (pixmaps): keep as a weak candidate */
            if (!best)
                best = e;
            continue;
        }
        if (!best || best->size <= 0) {
            best = e;
            continue;
        }
        /* prefer >= want (smallest such), else larger than best */
        if (e->size >= want) {
            if (best->size < want || e->size < best->size)
                best = e;
        } else if (best->size < want && e->size > best->size) {
            best = e;
        }
    }
    if (!best)
        return false;
    out->path = best->path;
    out->size = best->size;
    return true;
}

/* --------------------------------------------------------------- decode */

#ifdef XW_HAVE_PNG
static uint32_t *png_decode(const char *path, int *w, int *h) {
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path))
        return NULL;
    if (img.width < 1 || img.height < 1 || img.width > 512 ||
        img.height > 512) {
        png_image_finish_read(&img, NULL, NULL, 0, NULL);
        return NULL;
    }
    /* BGRA byte order reads back as 0xAARRGGBB words on LE hosts */
    img.format = PNG_FORMAT_BGRA;
    uint32_t *pix = malloc((size_t)img.width * img.height * 4);
    if (!pix) {
        png_image_finish_read(&img, NULL, NULL, 0, NULL);
        return NULL;
    }
    if (!png_image_finish_read(&img, NULL, pix, 0, NULL)) {
        free(pix);
        return NULL;
    }
    *w = (int)img.width;
    *h = (int)img.height;
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    /* big-endian: byte-swap BGRA -> ARGB word */
    for (size_t i = 0; i < (size_t)(*w) * (*h); i++) {
        uint32_t v = pix[i];
        pix[i] = (v >> 24) | ((v >> 8) & 0xff00) | ((v & 0xff00) << 8) |
                 (v << 24);
    }
#endif
    return pix;
}
#else
static uint32_t *png_decode(const char *path, int *w, int *h) {
    (void)path;
    (void)w;
    (void)h;
    return NULL; /* compiled without libpng: PNGs are skipped */
}
#endif

/* ----------------------------------------------------------- XPM parser */

/* a minimal X11 color-name table (the names that actually appear in
 * shipped .xpm icons); unknown names fall back to a mid gray */
static const struct {
    const char *name;
    uint32_t argb;
} xpm_colors[] = {
    {"none", 0x00000000},   {"black", 0xff000000},
    {"white", 0xffffffff},  {"red", 0xffff0000},
    {"green", 0xff00ff00},  {"blue", 0xff0000ff},
    {"yellow", 0xffffff00}, {"magenta", 0xffff00ff},
    {"cyan", 0xff00ffff},   {"gray", 0xff808080},
    {"grey", 0xff808080},   {"darkgray", 0xff404040},
    {"darkgrey", 0xff404040}, {"lightgray", 0xffd3d3d3},
    {"lightgrey", 0xffd3d3d3}, {"orange", 0xffffa500},
    {"brown", 0xffa52a2a},  {"pink", 0xffffc0cb},
    {"purple", 0xff800080}, {"navy", 0xff000080},
    {"navyblue", 0xff000080}, {"maroon", 0xff800000},
    {"olive", 0xff808000},  {"lime", 0xff00ff00},
    {"aqua", 0xff00ffff},   {"fuchsia", 0xffff00ff},
    {"silver", 0xffc0c0c0}, {"transparent", 0x00000000},
};

/* parse one XPM color value: #rrggbb / #rgb / #rrrrggggbbbb / gray
 * percent ("g#50") / symbolic name */
static uint32_t xpm_color_value(const char *val) {
    if (!val || !*val)
        return 0xff808080;
    if (val[0] == '#') {
        size_t n = strlen(val + 1);
        if (n == 3) {
            unsigned r, g, b;
            if (sscanf(val, "#%1x%1x%1x", &r, &g, &b) == 3)
                return 0xff000000u | (r * 17 << 16) | (g * 17 << 8) |
                       (b * 17);
            return 0xff808080;
        }
        if (n == 6) {
            unsigned r, g, b;
            if (sscanf(val, "#%2x%2x%2x", &r, &g, &b) == 3)
                return 0xff000000u | (r << 16) | (g << 8) | b;
            return 0xff808080;
        }
        if (n == 12) {
            unsigned r, g, b;
            if (sscanf(val, "#%4x%4x%4x", &r, &g, &b) == 3)
                return 0xff000000u | ((r >> 8) << 16) | ((g >> 8) << 8) |
                       (b >> 8);
            return 0xff808080;
        }
        return 0xff808080;
    }
    if (val[0] == 'g' && val[1] == '#') {
        /* gray percentage: "g#50" = 50% gray */
        int pct = atoi(val + 2);
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        unsigned v = (unsigned)(pct * 255 / 100);
        return 0xff000000u | (v << 16) | (v << 8) | v;
    }
    for (size_t i = 0; i < sizeof(xpm_colors) / sizeof(xpm_colors[0]); i++)
        if (strcasecmp(xpm_colors[i].name, val) == 0)
            return xpm_colors[i].argb;
    return 0xff808080; /* unknown symbolic name */
}

/* collect the C-string literals of an XPM3 file body into `out` */
static int xpm_strings(FILE *f, char out[][512], int max, int *maxlen) {
    int n = 0;
    *maxlen = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '"') {
            int len = 0;
            if (n < max) {
                while ((c = fgetc(f)) != EOF && c != '"') {
                    if (len < 511)
                        out[n][len++] = (char)c;
                }
                out[n][len] = 0;
                if (len > *maxlen)
                    *maxlen = len;
                if (len > 0)
                    n++;
                else
                    n++; /* empty strings still count positions */
            } else {
                while ((c = fgetc(f)) != EOF && c != '"')
                    ;
            }
        } else if (c == '}') {
            break;
        }
    }
    return n;
}

static uint32_t *xpm_decode(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    /* read the header word count: either XPM3 (C strings) or XPM2
     * (bare lines). XPM2 starts with "! XPM2". */
    char first[32] = {0};
    int c;
    size_t fi = 0;
    while ((c = fgetc(f)) != EOF && c != '\n' && fi < sizeof(first) - 1)
        first[fi++] = (char)c;
    rewind(f);

    int width = 0, height = 0, ncolors = 0, cpp = 1;
    uint32_t *pix = NULL;

    if (strncmp(first, "! XPM2", 6) == 0) {
        /* XPM2: header line then bare lines */
        char line[512];
        if (!fgets(line, sizeof(line), f))
            goto fail;
        if (sscanf(line, "%d %d %d %d", &width, &height, &ncolors, &cpp) != 4)
            goto fail;
        if (width < 1 || height < 1 || width > 512 || height > 512 ||
            ncolors < 1 || ncolors > 4096 || cpp < 1 || cpp > 4)
            goto fail;
        char (*palette)[8] = calloc((size_t)ncolors, 8);
        uint32_t *colors = calloc((size_t)ncolors, 4);
        if (!palette || !colors) {
            free(palette);
            free(colors);
            goto fail;
        }
        for (int i = 0; i < ncolors; i++) {
            if (!fgets(line, sizeof(line), f))
                break;
            /* "<chars> <key> <color>" (also 'c' quoted colors keep
             * spaces — the spec's quoted forms are rare; take the
             * remainder after the key as the value) */
            char key[8] = {0};
            int consumed = 0;
            if (sscanf(line, "%7s %7s %n", palette[i], key, &consumed) < 2)
                continue;
            char *val = line + consumed;
            /* trim trailing whitespace/newline */
            for (size_t L = strlen(val); L > 0 && (val[L - 1] == '\n' ||
                                                   val[L - 1] == ' ' ||
                                                   val[L - 1] == '\t' ||
                                                   val[L - 1] == '\r');)
                val[--L] = 0;
            colors[i] = xpm_color_value(val);
        }
        pix = calloc((size_t)width * height, 4);
        if (!pix) {
            free(palette);
            free(colors);
            goto fail;
        }
        for (int y = 0; y < height; y++) {
            if (!fgets(line, sizeof(line), f))
                break;
            for (int x = 0; x < width; x++) {
                if ((int)strlen(line) < (x + 1) * cpp)
                    break;
                const char *cell = line + x * cpp;
                for (int i = 0; i < ncolors; i++)
                    if (strncmp(cell, palette[i], (size_t)cpp) == 0) {
                        pix[y * width + x] = colors[i];
                        break;
                    }
            }
        }
        free(palette);
        free(colors);
        *w = width;
        *h = height;
        fclose(f);
        return pix;
    }

    /* XPM3: quoted strings until '}' (static: ~2 MB of line storage,
     * single-threaded clients only) */
    {
        static char vals[4200][512];
        int maxs = 0;
        int n = xpm_strings(f, vals, 4200, &maxs);
        fclose(f);
        if (n < 1)
            return NULL;
        if (sscanf(vals[0], "%d %d %d %d", &width, &height, &ncolors,
                   &cpp) != 4)
            return NULL;
        if (width < 1 || height < 1 || width > 512 || height > 512 ||
            ncolors < 1 || ncolors > 4200 || cpp < 1 || cpp > 4 ||
            n < 1 + ncolors + height)
            return NULL;
        /* palette keyed by first cpp chars of the color spec string */
        char (*pkeys)[8] = calloc((size_t)ncolors, 8);
        uint32_t *colors = calloc((size_t)ncolors, 4);
        pix = calloc((size_t)width * height, 4);
        if (!pkeys || !colors || !pix) {
            free(pkeys);
            free(colors);
            free(pix);
            return NULL;
        }
        for (int i = 0; i < ncolors; i++) {
            const char *s = vals[1 + i];
            snprintf(pkeys[i], 8, "%.4s", s);
            /* find the color value: after the "c"/"g"/"m"/"s" key */
            const char *key = s + cpp;
            while (*key == ' ')
                key++;
            while (*key && *key != ' ')
                key++; /* skip the key token */
            while (*key == ' ')
                key++;
            colors[i] = xpm_color_value(key);
        }
        for (int y = 0; y < height; y++) {
            const char *s = vals[1 + ncolors + y];
            for (int x = 0; x < width; x++) {
                if ((int)strlen(s) < (x + 1) * cpp)
                    break;
                for (int i = 0; i < ncolors; i++)
                    if (strncmp(s + x * cpp, pkeys[i], (size_t)cpp) == 0) {
                        pix[y * width + x] = colors[i];
                        break;
                    }
            }
        }
        free(pkeys);
        free(colors);
        *w = width;
        *h = height;
        return pix;
    }
fail:
    fclose(f);
    return NULL;
}

/* ---------------------------------------------------------------- scale */

/* alpha-weighted box downscale (premultiplied average prevents color
 * fringes on transparent borders) */
static uint32_t *downscale(const uint32_t *src, int sw, int sh, int dw,
                           int dh) {
    uint32_t *dst = calloc((size_t)dw * dh, 4);
    if (!dst)
        return NULL;
    for (int y = 0; y < dh; y++) {
        int sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * sw / dw, sx1 = (x + 1) * sw / dw;
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; sy++)
                for (int sx = sx0; sx < sx1 && sx < sw; sx++) {
                    uint32_t p = src[sy * sw + sx];
                    uint32_t al = p >> 24;
                    r += ((p >> 16) & 0xff) * al;
                    g += ((p >> 8) & 0xff) * al;
                    b += (p & 0xff) * al;
                    a += al;
                    n++;
                }
            if (n == 0)
                continue;
            if (a == 0) {
                dst[y * dw + x] = 0;
                continue;
            }
            uint32_t A = (uint32_t)(a / n);
            uint32_t R = (uint32_t)(r / a), G = (uint32_t)(g / a),
                     B = (uint32_t)(b / a);
            dst[y * dw + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    return dst;
}

static uint32_t *upscale(const uint32_t *src, int sw, int sh, int dw,
                         int dh) {
    uint32_t *dst = calloc((size_t)dw * dh, 4);
    if (!dst)
        return NULL;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        if (sy >= sh)
            sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            if (sx >= sw)
                sx = sw - 1;
            dst[y * dw + x] = src[sy * sw + sx];
        }
    }
    return dst;
}

/* ------------------------------------------------------------------ api */

static uint32_t *load_any(const char *path, int *w, int *h) {
    size_t len = strlen(path);
    if (len > 4) {
        const char *ext = path + len - 4;
        if (ext[0] == '.' && (ext[1] == 'p' || ext[1] == 'P') &&
            (ext[2] == 'n' || ext[2] == 'N') && (ext[3] == 'g' || ext[3] == 'G'))
            return png_decode(path, w, h);
        if (ext[0] == '.' && (ext[1] == 'x' || ext[1] == 'X') &&
            (ext[2] == 'p' || ext[2] == 'P') && (ext[3] == 'm' || ext[3] == 'M'))
            return xpm_decode(path, w, h);
    }
    /* unknown extension: try both decoders */
    uint32_t *p = png_decode(path, w, h);
    if (p)
        return p;
    return xpm_decode(path, w, h);
}

const struct xwc_icon *xwc_icon_get(const char *name, int size) {
    if (!name || !*name || size < 1 || size > 512)
        return NULL;

    char key[600];
    snprintf(key, sizeof(key), "%.540s@%d", name, size);
    const struct xwc_icon *hit = cache_get(key);
    if (hit)
        return hit;

    /* direct path? (icons carrying an absolute path) */
    char pathbuf[1024];
    const char *path = NULL;
    if (strchr(name, '/')) {
        path = name;
    } else {
        struct icon_file f;
        if (!index_find(name, size, &f)) {
            /* Icon= values sometimes carry an extension ("foo.png"):
             * the theme lookup wants the bare name too */
            char stripped[600];
            snprintf(stripped, sizeof(stripped), "%.560s", name);
            char *dot = strrchr(stripped, '.');
            if (dot && (strcasecmp(dot, ".png") == 0 ||
                        strcasecmp(dot, ".svg") == 0 ||
                        strcasecmp(dot, ".xpm") == 0)) {
                *dot = 0;
                if (index_find(stripped, size, &f))
                    path = f.path;
            }
            if (!path) {
                log_miss(key, name);
                return NULL;
            }
        } else {
            path = f.path;
        }
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (strchr(name, '/')) {
            /* an absolute name without extension: probe .png/.xpm */
            const char *exts[] = {".png", ".xpm"};
            for (int i = 0; i < 2; i++) {
                snprintf(pathbuf, sizeof(pathbuf), "%.960s%s", name, exts[i]);
                if (stat(pathbuf, &st) == 0 && S_ISREG(st.st_mode)) {
                    path = pathbuf;
                    break;
                }
            }
            if (path == name || path == NULL) {
                log_miss(key, name);
                return NULL;
            }
        } else {
            log_miss(key, name);
            return NULL;
        }
    }

    int w = 0, h = 0;
    uint32_t *pix = load_any(path, &w, &h);
    if (!pix) {
        log_miss(key, name);
        return NULL;
    }

    /* scale into a square size x size cell (icons keep aspect by
     * letterboxing in xwc_draw_icon; here the surface is the largest
     * square-fitting box) */
    uint32_t *out_pix = pix;
    int ow = w, oh = h;
    if (w > size || h > size) {
        int dw = w > h ? size : (int)((int64_t)w * size / h);
        int dh = h > w ? size : (int)((int64_t)h * size / w);
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        uint32_t *d = downscale(pix, w, h, dw, dh);
        free(pix);
        if (!d)
            return NULL;
        out_pix = d;
        ow = dw;
        oh = dh;
    } else if (w < size && h < size && (size - w > size / 4 ||
                                        size - h > size / 4)) {
        /* small icon requested big: only upscale when the gap is
         * significant (avoids soft blur for near-misses) */
        int dw = w > h ? size : (int)((int64_t)w * size / h);
        int dh = h > w ? size : (int)((int64_t)h * size / w);
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        uint32_t *u = upscale(pix, w, h, dw, dh);
        free(pix);
        if (!u)
            return NULL;
        out_pix = u;
        ow = dw;
        oh = dh;
    }

    cache_put(key, out_pix, ow, oh);
    return cache_get(key);
}
