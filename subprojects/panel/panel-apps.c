/* panel-apps.c — see panel-apps.h. Everything here is client-side and
 * dependency-free (libc only): the panel must work on any distro with
 * nothing but freedesktop metadata present. */
#include "panel-apps.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------ locale */

/* "en_US.UTF-8" -> "en_US" and "en" (mod_ / mod_short_), empty when no
 * localization is active */
static void locale_variants(char *mod, size_t mod_n, char *mod_short,
                            size_t short_n) {
    mod[0] = mod_short[0] = 0;
    const char *vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    const char *loc = NULL;
    for (size_t i = 0; i < 3 && !(loc && *loc); i++)
        loc = getenv(vars[i]);
    if (!loc || !*loc || strcmp(loc, "C") == 0 || strcmp(loc, "POSIX") == 0)
        return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.60s", loc);
    char *dot = strchr(buf, '.');
    if (dot)
        *dot = 0; /* strip the encoding */
    char *at = strchr(buf, '@');
    if (at)
        *at = 0; /* strip the modifier */
    snprintf(mod, mod_n, "%.31s", buf);
    if (strlen(buf) >= 2)
        snprintf(mod_short, short_n, "%.2s", buf);
}

/* ------------------------------------------------------------ reading */

/* one [Desktop Entry] group loaded into memory: a line array (the
 * files are small; one read + in-place field lookup beats one fopen
 * per field) */
struct entry {
    char *lines[192];
    int n_lines;
    char *buf;
};

static bool entry_load(struct entry *e, const char *path) {
    memset(e, 0, sizeof(*e));
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 8 || sz > 256 * 1024) { /* empty or absurdly large */
        fclose(f);
        return false;
    }
    e->buf = malloc((size_t)sz + 1);
    if (!e->buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(e->buf, 1, (size_t)sz, f);
    fclose(f);
    e->buf[got] = 0;

    bool in_group = false;
    char *p = e->buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        /* strip CR */
        size_t len = strlen(p);
        if (len && p[len - 1] == '\r')
            p[len - 1] = 0;
        if (p[0] == '[') {
            in_group = strncmp(p, "[Desktop Entry]", 15) == 0;
        } else if (in_group && p[0] && p[0] != '#') {
            if (e->n_lines < 192)
                e->lines[e->n_lines++] = p;
        }
        p = nl ? nl + 1 : NULL;
    }
    return true;
}

static void entry_free(struct entry *e) {
    free(e->buf);
    e->buf = NULL;
    e->n_lines = 0;
}

static char *entry_lookup(struct entry *e, const char *key) {
    char full[96];
    char mod[32], mod_short[8];
    locale_variants(mod, sizeof(mod), mod_short, sizeof(mod_short));

    /* localized first: Name[de_DE], then Name[de] */
    for (int pass = 0; pass < 2; pass++) {
        const char *loc = pass == 0 ? mod : mod_short;
        if (!*loc)
            continue;
        snprintf(full, sizeof(full), "%s[%s]", key, loc);
        size_t flen = strlen(full);
        for (int i = 0; i < e->n_lines; i++) {
            if (strncmp(e->lines[i], full, flen) == 0 &&
                e->lines[i][flen] == '=')
                return e->lines[i] + flen + 1;
        }
    }
    size_t klen = strlen(key);
    for (int i = 0; i < e->n_lines; i++) {
        if (strncmp(e->lines[i], key, klen) == 0 &&
            e->lines[i][klen] == '=')
            return e->lines[i] + klen + 1;
    }
    return NULL;
}

static bool entry_bool(struct entry *e, const char *key) {
    char *v = entry_lookup(e, key);
    return v && (strcmp(v, "true") == 0 || strcmp(v, "True") == 0 ||
                 strcmp(v, "1") == 0);
}

/* ------------------------------------------------------------ paths */

bool xwapp_path_has(const char *name) {
    if (!name || !*name)
        return false;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char probe[1024];
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < sizeof(probe) - 200) {
            snprintf(probe, sizeof(probe), "%.*s/%.180s", (int)len, p, name);
            if (access(probe, X_OK) == 0)
                return true;
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return false;
}

static void copy_str(char *dst, size_t n, const char *src) {
    /* byte loop, not snprintf: the destination and source can be two
     * fields of the same struct, and snprintf's restrict contract
     * makes GCC reject that pattern (-Werror=restrict) */
    if (!n)
        return;
    if (!src)
        src = "";
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* ------------------------------------------------------- categories */

static const struct {
    const char *name;
    const char *icon;
} cat_meta[XWAPP_CAT_COUNT] = {
    [XWAPP_CAT_FAVORITES] = {"Favorites", "user-bookmarks"},
    [XWAPP_CAT_ALL] = {"All Applications", "applications-all"},
    [XWAPP_CAT_ACCESSORIES] = {"Accessories", "applications-accessories"},
    [XWAPP_CAT_DEVELOPMENT] = {"Development", "applications-development"},
    [XWAPP_CAT_EDUCATION] = {"Education", "applications-science"},
    [XWAPP_CAT_GAMES] = {"Games", "applications-games"},
    [XWAPP_CAT_GRAPHICS] = {"Graphics", "applications-graphics"},
    [XWAPP_CAT_MULTIMEDIA] = {"Multimedia", "applications-multimedia"},
    [XWAPP_CAT_NETWORK] = {"Internet", "applications-internet"},
    [XWAPP_CAT_OFFICE] = {"Office", "applications-office"},
    [XWAPP_CAT_SCIENCE] = {"Science", "applications-science"},
    [XWAPP_CAT_SETTINGS] = {"Settings", "preferences-system"},
    [XWAPP_CAT_SYSTEM] = {"System", "applications-system"},
    [XWAPP_CAT_TERMINAL] = {"Terminal Emulators", "utilities-terminal"},
    [XWAPP_CAT_OTHER] = {"Other", "applications-other"},
};

const char *xwapp_cat_name(int cat) {
    return (cat >= 0 && cat < XWAPP_CAT_COUNT) ? cat_meta[cat].name : "?";
}

const char *xwapp_cat_icon(int cat) {
    return (cat >= 0 && cat < XWAPP_CAT_COUNT) ? cat_meta[cat].icon
                                                : "applications-other";
}

/* categories are semicolon-separated; the spec's main categories map
 * onto the XFCE-style groups. Precedence: specific main categories
 * win over the generic Utility; TerminalEmulator only claims apps
 * that are nothing else. */
static int resolve_category(const char *categories) {
    if (!categories || !*categories)
        return XWAPP_CAT_OTHER;
    char cats[XWAPP_COMMENT_MAX];
    copy_str(cats, sizeof(cats), categories);

    static const struct {
        const char *token;
        int cat;
    } prio[] = {
        {"Settings", XWAPP_CAT_SETTINGS},
        {"Development", XWAPP_CAT_DEVELOPMENT},
        {"Education", XWAPP_CAT_EDUCATION},
        {"Science", XWAPP_CAT_SCIENCE},
        {"Game", XWAPP_CAT_GAMES},
        {"Graphics", XWAPP_CAT_GRAPHICS},
        {"Network", XWAPP_CAT_NETWORK},
        {"Office", XWAPP_CAT_OFFICE},
        {"AudioVideo", XWAPP_CAT_MULTIMEDIA},
        {"Audio", XWAPP_CAT_MULTIMEDIA},
        {"Video", XWAPP_CAT_MULTIMEDIA},
        {"Utility", XWAPP_CAT_ACCESSORIES},
        {"System", XWAPP_CAT_SYSTEM},
        {"FileManager", XWAPP_CAT_SYSTEM},
        {"TerminalEmulator", XWAPP_CAT_TERMINAL},
    };
    /* Settings beats Utility even when both appear (a settings tool is
     * a settings tool); everything else: first specific match wins */
    bool has_utility = false;
    char *save = NULL;
    for (char *tok = strtok_r(cats, ";", &save); tok;
         tok = strtok_r(NULL, ";", &save)) {
        if (strcmp(tok, "Utility") == 0)
            has_utility = true;
        if (strcmp(tok, "Settings") == 0)
            return XWAPP_CAT_SETTINGS;
    }
    for (size_t i = 0; i < sizeof(prio) / sizeof(prio[0]); i++) {
        if (strcmp(prio[i].token, "Utility") == 0)
            continue; /* handled below: lowest specific priority */
        snprintf(cats, sizeof(cats), "%s", categories);
        save = NULL;
        for (char *tok = strtok_r(cats, ";", &save); tok;
             tok = strtok_r(NULL, ";", &save))
            if (strcmp(tok, prio[i].token) == 0)
                return prio[i].cat;
    }
    if (has_utility)
        return XWAPP_CAT_ACCESSORIES;
    return XWAPP_CAT_OTHER;
}

/* ------------------------------------------------------------- scan */

/* directory mtime cache: rescans are only performed when an
 * applications directory actually changed */
struct dir_stamp {
    char path[XWAPP_PATH_MAX];
    time_t mtime;
};

static struct dir_stamp g_stamps[24];
static int g_n_stamps;

static bool dir_changed(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    for (int i = 0; i < g_n_stamps; i++)
        if (strcmp(g_stamps[i].path, path) == 0)
            return g_stamps[i].mtime != st.st_mtime;
    return true; /* never seen */
}

static void stamp_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return;
    for (int i = 0; i < g_n_stamps; i++)
        if (strcmp(g_stamps[i].path, path) == 0) {
            g_stamps[i].mtime = st.st_mtime;
            return;
        }
    if (g_n_stamps < 24) {
        snprintf(g_stamps[g_n_stamps].path, XWAPP_PATH_MAX, "%.480s",
                 path);
        g_stamps[g_n_stamps].mtime = st.st_mtime;
        g_n_stamps++;
    }
}

static void casefold(char *s) {
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

static int app_cmp(const void *a, const void *b) {
    const struct xwapp *x = a, *y = b;
    int c = strcmp(x->sort_key, y->sort_key);
    if (c)
        return c;
    return strcmp(x->desktop_id, y->desktop_id);
}

/* accept a .desktop entry: visibility rules per the spec + the panel's
 * practical needs */
static bool entry_visible(struct entry *e) {
    char *type = entry_lookup(e, "Type");
    if (!type || strcmp(type, "Application") != 0)
        return false;
    char *name = entry_lookup(e, "Name");
    char *exec = entry_lookup(e, "Exec");
    if (!name || !*name || !exec || !*exec)
        return false;
    if (entry_bool(e, "NoDisplay") || entry_bool(e, "Hidden"))
        return false;
    /* TryExec: hide when the program is not present */
    char *try_exec = entry_lookup(e, "TryExec");
    if (try_exec && *try_exec && !xwapp_path_has(try_exec))
        return false;
    /* OnlyShowIn: entries restricting their desktops; we are an XFCE
     * desktop (XDG_CURRENT_DESKTOP from the session) */
    const char *cur_desktop = getenv("XDG_CURRENT_DESKTOP");
    char desk[64] = {0};
    if (cur_desktop && *cur_desktop) {
        /* the var may be a colon list: use the first component */
        snprintf(desk, sizeof(desk), "%.60s", cur_desktop);
        char *colon = strchr(desk, ':');
        if (colon)
            *colon = 0;
    } else {
        snprintf(desk, sizeof(desk), "XFCE");
    }
    char *only = entry_lookup(e, "OnlyShowIn");
    if (only && *only) {
        char list[XWAPP_COMMENT_MAX];
        copy_str(list, sizeof(list), only);
        char *save = NULL;
        bool ok = false;
        for (char *tok = strtok_r(list, ";", &save); tok;
             tok = strtok_r(NULL, ";", &save))
            if (strcmp(tok, desk) == 0 || strcmp(tok, "XFCE") == 0)
                ok = true;
        if (!ok)
            return false;
    }
    char *notshow = entry_lookup(e, "NotShowIn");
    if (notshow && *notshow) {
        char list[XWAPP_COMMENT_MAX];
        copy_str(list, sizeof(list), notshow);
        char *save = NULL;
        for (char *tok = strtok_r(list, ";", &save); tok;
             tok = strtok_r(NULL, ";", &save))
            if (strcmp(tok, desk) == 0)
                return false;
    }
    return true;
}

static void scan_one_dir(struct xwapp_db *db, const char *dir,
                         char seen[][XWAPP_ID_MAX], int *n_seen,
                         int seen_cap) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) && db->n_apps < XWAPP_MAX) {
        if (de->d_name[0] == '.')
            continue;
        size_t len = strlen(de->d_name);
        if (len < 9 || strcmp(de->d_name + len - 8, ".desktop") != 0)
            continue;
        /* subdirectories are not scanned (no desktop-file dirs v1) */
        char id[XWAPP_ID_MAX];
        snprintf(id, sizeof(id), "%.*s", (int)(len - 8), de->d_name);
        bool dup = false;
        for (int i = 0; i < *n_seen; i++)
            if (strcmp(seen[i], id) == 0) {
                dup = true;
                break;
            }
        if (dup)
            continue; /* user entry already shadowed this id */

        char path[XWAPP_PATH_MAX];
        snprintf(path, sizeof(path), "%.240s/%.140s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        struct entry e;
        if (!entry_load(&e, path))
            continue;
        if (!entry_visible(&e)) {
            entry_free(&e);
            if (*n_seen < seen_cap)
                copy_str(seen[(*n_seen)++], XWAPP_ID_MAX, id);
            continue;
        }
        struct xwapp *a = &db->apps[db->n_apps++];
        copy_str(a->desktop_id, sizeof(a->desktop_id), id);
        copy_str(a->name, sizeof(a->name), entry_lookup(&e, "Name"));
        copy_str(a->generic, sizeof(a->generic),
                 entry_lookup(&e, "GenericName"));
        copy_str(a->comment, sizeof(a->comment), entry_lookup(&e, "Comment"));
        copy_str(a->exec, sizeof(a->exec), entry_lookup(&e, "Exec"));
        copy_str(a->icon, sizeof(a->icon), entry_lookup(&e, "Icon"));
        copy_str(a->categories, sizeof(a->categories),
                 entry_lookup(&e, "Categories"));
        copy_str(a->path, sizeof(a->path), path);
        a->terminal = entry_bool(&e, "Terminal");
        a->cat = resolve_category(a->categories);
        copy_str(a->sort_key, sizeof(a->sort_key), a->name);
        casefold(a->sort_key);
        entry_free(&e);

        if (*n_seen < seen_cap)
            copy_str(seen[(*n_seen)++], XWAPP_ID_MAX, id);
    }
    closedir(d);
    stamp_dir(dir);
}

int xwapp_db_scan(struct xwapp_db *db) {
    if (!db)
        return -1;
    /* collect the directories (user first: shadowing) */
    char dirs[8][XWAPP_PATH_MAX];
    int n_dirs = 0;
    char seen[XWAPP_MAX][XWAPP_ID_MAX];
    (void)seen;

    const char *xh = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xh && *xh)
        snprintf(dirs[n_dirs++], XWAPP_PATH_MAX, "%.460s/applications", xh);
    else if (home && *home)
        snprintf(dirs[n_dirs++], XWAPP_PATH_MAX,
                 "%.240s/.local/share/applications", home);

    const char *xd = getenv("XDG_DATA_DIRS");
    if (!xd || !*xd)
        xd = "/usr/local/share:/usr/share";
    char xdcopy[768];
    snprintf(xdcopy, sizeof(xdcopy), "%.700s", xd);
    char *save = NULL;
    for (char *tok = strtok_r(xdcopy, ":", &save);
         tok && n_dirs < 6; tok = strtok_r(NULL, ":", &save))
        snprintf(dirs[n_dirs++], XWAPP_PATH_MAX, "%.460s/applications", tok);

    /* unchanged tree: keep the current database */
    bool any_changed = false;
    for (int i = 0; i < n_dirs; i++)
        if (dir_changed(dirs[i]))
            any_changed = true;
    if (!any_changed && db->n_apps > 0)
        return 0;

    db->n_apps = 0;
    static char seen_ids[2048][XWAPP_ID_MAX];
    int n_seen = 0;
    for (int i = 0; i < n_dirs; i++)
        scan_one_dir(db, dirs[i], seen_ids, &n_seen, 2048);

    qsort(db->apps, (size_t)db->n_apps, sizeof(db->apps[0]), app_cmp);

    /* rebuild the category indexes */
    for (int c = 0; c < XWAPP_CAT_COUNT; c++)
        db->cat_n[c] = 0;
    for (int i = 0; i < db->n_apps; i++) {
        int c = db->apps[i].cat;
        if (c >= XWAPP_CAT_ALL && c < XWAPP_CAT_COUNT &&
            db->cat_n[c] < XWAPP_MAX)
            db->cat_idx[c][db->cat_n[c]++] = i;
    }
    return 0;
}

/* --------------------------------------------------------- queries */

int xwapp_db_count(const struct xwapp_db *db) {
    return db ? db->n_apps : 0;
}

const struct xwapp *xwapp_at(const struct xwapp_db *db, int i) {
    return (db && i >= 0 && i < db->n_apps) ? &db->apps[i] : NULL;
}

const struct xwapp *xwapp_by_id(const struct xwapp_db *db,
                                const char *desktop_id) {
    if (!db || !desktop_id)
        return NULL;
    for (int i = 0; i < db->n_apps; i++)
        if (strcmp(db->apps[i].desktop_id, desktop_id) == 0)
            return &db->apps[i];
    return NULL;
}

void xwapp_db_set_favorites(struct xwapp_db *db, const char *const *ids,
                            int n) {
    if (!db)
        return;
    db->n_favorites = 0;
    for (int i = 0; i < n && db->n_favorites < 64; i++)
        copy_str(db->favorites[db->n_favorites++], XWAPP_ID_MAX, ids[i]);
}

int xwapp_db_favorites(const struct xwapp_db *db,
                       const char *out[][XWAPP_ID_MAX]) {
    (void)db;
    (void)out;
    return 0; /* reserved (favorites resolve through cat queries) */
}

int xwapp_cat_count(const struct xwapp_db *db, int cat) {
    if (!db)
        return 0;
    if (cat == XWAPP_CAT_FAVORITES) {
        int n = 0;
        for (int i = 0; i < db->n_favorites; i++)
            if (xwapp_by_id(db, db->favorites[i]))
                n++;
        return n;
    }
    if (cat == XWAPP_CAT_ALL)
        return db->n_apps;
    if (cat < 0 || cat >= XWAPP_CAT_COUNT)
        return 0;
    return db->cat_n[cat];
}

const struct xwapp *xwapp_cat_at(const struct xwapp_db *db, int cat, int i) {
    if (!db || i < 0)
        return NULL;
    if (cat == XWAPP_CAT_FAVORITES) {
        int k = 0;
        for (int f = 0; f < db->n_favorites; f++) {
            const struct xwapp *a = xwapp_by_id(db, db->favorites[f]);
            if (a) {
                if (k == i)
                    return a;
                k++;
            }
        }
        return NULL;
    }
    if (cat == XWAPP_CAT_ALL)
        return xwapp_at(db, i);
    if (cat < 0 || cat >= XWAPP_CAT_COUNT || i >= db->cat_n[cat])
        return NULL;
    return &db->apps[db->cat_idx[cat][i]];
}

int xwapp_search(const struct xwapp_db *db, const char *needle, int *out,
                 int cap) {
    if (!db || !needle || !*needle)
        return 0;
    char fold[XWAPP_NAME_MAX + XWAPP_COMMENT_MAX];
    snprintf(fold, sizeof(fold), "%s", needle);
    casefold(fold);
    int n = 0;
    for (int i = 0; i < db->n_apps && n < cap; i++) {
        const struct xwapp *a = &db->apps[i];
        char hay[XWAPP_COMMENT_MAX * 2 + XWAPP_ID_MAX];
        snprintf(hay, sizeof(hay), "%s %s %s %s", a->name, a->generic,
                 a->comment, a->desktop_id);
        casefold(hay);
        if (strstr(hay, fold))
            out[n++] = i;
    }
    return n;
}

/* ------------------------------------------------------ exec parsing */

/* Append one character to the current token; a field code may expand
 * into a whole argument (icon name, app name), so codes are resolved
 * as tokens of their own. */
struct exec_parser {
    char args[XWAPP_MAX_ARGS][XWAPP_ARG_MAX];
    int n_args;
    char tok[XWAPP_ARG_MAX];
    int n_tok;
    bool in_token;
};

static void ep_flush(struct exec_parser *p) {
    if (!p->in_token)
        return;
    if (p->n_args < XWAPP_MAX_ARGS) {
        copy_str(p->args[p->n_args], XWAPP_ARG_MAX, p->tok);
        p->n_args++;
    }
    p->n_tok = 0;
    p->tok[0] = 0;
    p->in_token = false;
}

static void ep_push(struct exec_parser *p, char c) {
    if (p->n_tok < XWAPP_ARG_MAX - 1) {
        p->tok[p->n_tok++] = c;
        p->tok[p->n_tok] = 0;
    }
    p->in_token = true;
}

static void ep_push_str(struct exec_parser *p, const char *s) {
    for (; s && *s; s++)
        ep_push(p, *s);
}

int xwapp_exec_argv(const char *exec, const char *icon, const char *name,
                    const char *desktop_path, char args[][XWAPP_ARG_MAX],
                    int max) {
    if (!exec || !*exec)
        return -1;
    struct exec_parser p = {0};

    const char *s = exec;
    bool in_quotes = false;
    while (*s) {
        char c = *s;
        if (c == '"') {
            in_quotes = !in_quotes;
            s++;
            p.in_token = true; /* "" produces an empty argument */
            continue;
        }
        if (c == '\\') {
            char next = s[1];
            if (!next)
                return -1; /* dangling backslash: malformed */
            if (in_quotes) {
                /* inside quotes only \" \$ \` \\ collapse; \x for other
                 * x stays backslash + x */
                if (next == '"' || next == '$' || next == '`' ||
                    next == '\\')
                    ep_push(&p, next);
                else {
                    ep_push(&p, '\\');
                    ep_push(&p, next);
                }
            } else {
                /* outside quotes a backslash escapes the next char
                 * (space, quotes, reserved symbols) */
                ep_push(&p, next);
            }
            s += 2;
            continue;
        }
        if (!in_quotes && (c == ' ' || c == '\t' || c == '\n')) {
            ep_flush(&p);
            s++;
            continue;
        }
        if (!in_quotes && (c == '\'' || c == '>' || c == '<' || c == '|' ||
                           c == '&' || c == ';' || c == '$' || c == '`')) {
            /* reserved symbols must be quoted per the spec; tolerate
             * them as literals rather than dropping the entry */
            ep_push(&p, c);
            s++;
            continue;
        }
        if (!in_quotes && c == '%') {
            char code = s[1];
            if (!code)
                return -1; /* dangling %: malformed */
            switch (code) {
            case '%': /* %% -> literal % */
                ep_push(&p, '%');
                break;
            case 'f': case 'F': case 'u': case 'U':
                /* file/url lists: no file context in a launcher; the
                 * spec says launchers may drop them */
                break;
            case 'd': case 'D': case 'n': case 'N': case 'v': case 'm':
                /* deprecated codes: ignored */
                break;
            case 'i': /* %i -> --icon <icon> */
                if (icon && *icon) {
                    ep_flush(&p);
                    ep_push_str(&p, "--icon");
                    ep_flush(&p);
                    ep_push_str(&p, icon);
                }
                break;
            case 'c': /* %c -> translated name */
                if (name && *name)
                    ep_push_str(&p, name);
                break;
            case 'k': /* %k -> the .desktop file's location; the spec
                         allows file: URIs — local files pass as plain
                         paths (the common reading, and what file
                         managers hand to apps) */
                if (desktop_path && *desktop_path &&
                    p.n_tok < XWAPP_ARG_MAX) {
                    ep_flush(&p);
                    ep_push_str(&p, desktop_path);
                    ep_flush(&p);
                }
                break;
            default:
                /* unknown code: keep it verbatim (spec: invalid fields
                 * make the entry invalid, but real files carry typos;
                 * the safest reading is to pass them through) */
                ep_push(&p, '%');
                ep_push(&p, code);
                break;
            }
            s += 2;
            continue;
        }
        ep_push(&p, c);
        s++;
    }
    if (in_quotes)
        return -1; /* unterminated quote */
    ep_flush(&p);

    if (p.n_args < 1)
        return -1;
    if (p.n_args > max)
        p.n_args = max;
    for (int i = 0; i < p.n_args; i++)
        copy_str(args[i], XWAPP_ARG_MAX, p.args[i]);
    return p.n_args;
}

/* ---------------------------------------------------- terminal wrap */

static const struct {
    const char *bin;
    int style;
} term_styles[] = {
    {"gnome-terminal", XWAPP_TERM_DASHDASH},
    {"gnome-terminal-wrapper", XWAPP_TERM_DASHDASH},
    {"wezterm", XWAPP_TERM_DASHDASH},
    {"kitty", XWAPP_TERM_POSITIONAL},
    {"foot", XWAPP_TERM_POSITIONAL},
    {"xfce4-terminal", XWAPP_TERM_X},
    {"terminator", XWAPP_TERM_X},
    {"konsole", XWAPP_TERM_E_REST},
    {"alacritty", XWAPP_TERM_E_REST},
    {"lxterminal", XWAPP_TERM_E_REST},
    {"terminology", XWAPP_TERM_E_REST},
    {"st", XWAPP_TERM_E_REST},
    {"urxvt", XWAPP_TERM_E_REST},
    {"rxvt", XWAPP_TERM_E_REST},
    {"xterm", XWAPP_TERM_E_REST},
    {"qterminal", XWAPP_TERM_E_REST},
    {"tilix", XWAPP_TERM_E_REST},
    {"x-terminal-emulator", XWAPP_TERM_E_REST},
};

bool xwapp_resolve_terminal(char *out, size_t n, int *style) {
    const char *env = getenv("XW_TERMINAL");
    if (env && *env) {
        /* the configured terminal wins outright: first word must be
         * runnable (it may carry arguments of its own) */
        char first[256];
        copy_str(first, sizeof(first), env);
        char *sp = strchr(first, ' ');
        if (sp)
            *sp = 0;
        if (xwapp_path_has(first) || strchr(first, '/')) {
            copy_str(out, n, env);
            if (style) {
                const char *base = strrchr(first, '/');
                base = base ? base + 1 : first;
                *style = XWAPP_TERM_E_REST;
                for (size_t i = 0; i < sizeof(term_styles) /
                                        sizeof(term_styles[0]);
                     i++)
                    if (strcmp(base, term_styles[i].bin) == 0)
                        *style = term_styles[i].style;
            }
            return true;
        }
    }
    static const char *const candidates[] = {
        "xfce4-terminal", "konsole",     "gnome-terminal", "kitty",
        "alacritty",      "foot",       "wezterm",        "lxterminal",
        "terminology",    "st",         "xterm",          "x-terminal-emulator",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (xwapp_path_has(candidates[i])) {
            copy_str(out, n, candidates[i]);
            if (style) {
                *style = XWAPP_TERM_E_REST;
                for (size_t k = 0; k < sizeof(term_styles) /
                                        sizeof(term_styles[0]);
                     k++)
                    if (strcmp(candidates[i], term_styles[k].bin) == 0)
                        *style = term_styles[k].style;
            }
            return true;
        }
    }
    return false;
}

int xwapp_launch_argv(const struct xwapp *a, char args[][XWAPP_ARG_MAX],
                      int max, char *err, size_t err_n) {
    if (!a || !a->exec[0])
        return -1;
    char argv[XWAPP_MAX_ARGS][XWAPP_ARG_MAX];
    int n = xwapp_exec_argv(a->exec, a->icon[0] ? a->icon : NULL, a->name,
                            a->path[0] ? a->path : NULL, argv,
                            XWAPP_MAX_ARGS);
    if (n < 1) {
        snprintf(err, err_n, "malformed Exec line");
        return -1;
    }
    if (!a->terminal) {
        int m = n < max ? n : max;
        for (int i = 0; i < m; i++)
            copy_str(args[i], XWAPP_ARG_MAX, argv[i]);
        return m;
    }
    /* Terminal=true: host the command in a terminal.
     *
     * -x / -- / positional styles take the application argv DIRECTLY
     *   (no shell anywhere in the chain — the args are already
     *   spec-parsed tokens).
     * - -e style terminals (xterm family) historically want the whole
     *   command as ONE string; the already-tokenized argv is joined
     *   with proper quoting for that single argument (the TERMINAL
     *   word-splits it itself; no shell is involved). */
    char term[256];
    int style = XWAPP_TERM_E_REST;
    if (!xwapp_resolve_terminal(term, sizeof(term), &style)) {
        snprintf(err, err_n,
                 "Terminal=true but no terminal found (set $XW_TERMINAL)");
        return -1;
    }
    /* the terminal may itself carry arguments (XW_TERMINAL="foot -c x") */
    char targs[XWAPP_MAX_ARGS][XWAPP_ARG_MAX];
    int nt = xwapp_exec_argv(term, NULL, NULL, NULL, targs,
                             XWAPP_MAX_ARGS);
    if (nt < 1)
        nt = 0;
    int i = 0;
    for (int k = 0; k < nt && i < max; k++)
        copy_str(args[i++], XWAPP_ARG_MAX, targs[k]);
    if (i >= max)
        return -1;
    if (style == XWAPP_TERM_DASHDASH) {
        copy_str(args[i++], XWAPP_ARG_MAX, "--");
    } else if (style == XWAPP_TERM_X) {
        copy_str(args[i++], XWAPP_ARG_MAX, "-x");
    } else if (style == XWAPP_TERM_E_REST) {
        copy_str(args[i++], XWAPP_ARG_MAX, "-e");
    }
    if (style == XWAPP_TERM_E_REST) {
        /* single command string for the -e family */
        char inner[1024];
        if (!xwapp_argv_to_shell((const char(*)[XWAPP_ARG_MAX])argv, n,
                                 inner, sizeof(inner))) {
            snprintf(err, err_n, "command line too long for the terminal");
            return -1;
        }
        if (i < max)
            copy_str(args[i++], XWAPP_ARG_MAX, inner);
        return i;
    }
    /* X and POSITIONAL styles: the application argv goes straight in */
    for (int k = 0; k < n && i < max; k++)
        copy_str(args[i++], XWAPP_ARG_MAX, argv[k]);
    return i;
}

/* ------------------------------------------------------ serialization */

bool xwapp_argv_to_shell(const char args[][XWAPP_ARG_MAX], int n, char *out,
                         size_t out_n) {
    if (!out || out_n < 4)
        return false;
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        const char *a = args[i];
        if (!*a)
            continue;
        bool needs_quote = strpbrk(a, " \t\n\"'`$\\|&;()<>*?[]#~") != NULL;
        if (i > 0)
            used += (size_t)snprintf(out + used, out_n - used, " ");
        if (needs_quote) {
            used += (size_t)snprintf(out + used, out_n - used, "'");
            for (const char *c = a; *c && used + 4 < out_n; c++) {
                if (*c == '\'')
                    used += (size_t)snprintf(out + used, out_n - used,
                                             "'\\''");
                else {
                    out[used++] = *c;
                    out[used] = 0;
                }
            }
            used += (size_t)snprintf(out + used, out_n - used, "'");
        } else {
            used += (size_t)snprintf(out + used, out_n - used, "%s", a);
        }
        if (used >= out_n - 2)
            return false; /* does not fit */
    }
    return *out != 0;
}
