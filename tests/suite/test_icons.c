/* test_icons.c — client-library foundations: the second font raster and
 * the XDG icon pipeline (index building, priority, size selection,
 * XPM/PNG decode, rescale, cache, blitting). All filesystem state is
 * hermetic: XDG_* and HOME point at per-test temp directories, so the
 * results never depend on what the host machine has installed. */
#include "xwtest.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------ helpers */

static char t_dir[256];

static void t_mkdir(const char *path) {
    mkdir(path, 0755);
}

static void mkdirs(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* create each '/'-terminated prefix (mkdir is not recursive;
     * EEXIST is fine) */
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755); /* the full path itself */
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    XWT_ASSERT(f);
    fputs(content, f);
    fclose(f);
}

/* a 4x4 XPM: 'X' = pure opaque red, '.' = fully transparent */
static const char *XPM_RED4 =
    "/* XPM */\n"
    "static char *red4[] = {\n"
    "\"4 4 2 1\",\n"
    "\"X c #FF0000\",\n"
    "\". s none\",\n"
    "\"X.X.\",\n"
    "\"X..X\",\n"
    "\"..XX\",\n"
    "\".X.X\",\n"
    "};\n";

static const char *XPM_BLUE4 =
    "/* XPM */\n"
    "static char *blue4[] = {\n"
    "\"4 4 2 1\",\n"
    "\"X c #0000FF\",\n"
    "\". c none\",\n"
    "\"XXXX\",\n"
    "\"XXXX\",\n"
    "\"XXXX\",\n"
    "\"XXXX\",\n"
    "};\n";

/* locate a repo asset from either run directory (repo root / tests/) */
static const char *asset(const char *name) {
    static char path[300];
    if (access(name, R_OK) == 0)
        return name;
    snprintf(path, sizeof(path), "tests/assets/%s", name);
    if (access(path, R_OK) == 0)
        return path;
    snprintf(path, sizeof(path), "../tests/assets/%s", name);
    if (access(path, R_OK) == 0)
        return path;
    return NULL;
}

/* env save/restore: the icon module reads XDG paths + HOME lazily at
 * index build; tests redirect them and flush the index between cases */
static char *g_home, *g_xdh, *g_xdd;

static void env_snapshot(void) {
    g_home = getenv("HOME");
    g_xdh = getenv("XDG_DATA_HOME");
    g_xdd = getenv("XDG_DATA_DIRS");
}

static void env_redirect(const char *home, const char *data_home,
                         const char *data_dirs) {
    setenv("HOME", home, 1);
    if (data_home)
        setenv("XDG_DATA_HOME", data_home, 1);
    else
        unsetenv("XDG_DATA_HOME");
    if (data_dirs)
        setenv("XDG_DATA_DIRS", data_dirs, 1);
    else
        unsetenv("XDG_DATA_DIRS");
    xwc_icon_flush();
}

static void env_restore(void) {
    setenv("HOME", g_home ? g_home : "", 1);
    if (g_xdh)
        setenv("XDG_DATA_HOME", g_xdh, 1);
    else
        unsetenv("XDG_DATA_HOME");
    if (g_xdd)
        setenv("XDG_DATA_DIRS", g_xdd, 1);
    else
        unsetenv("XDG_DATA_DIRS");
    xwc_icon_flush();
}

/* --------------------------------------------------------------- tests */

static void test_font2(struct xwt_ctx *t) {
    (void)t;
    /* metrics: the second raster is strictly larger */
    XWT_CHECK(XWC_LINE2_H > XWC_LINE_H, "line2 %d > line %d", XWC_LINE2_H,
              XWC_LINE_H);
    XWT_CHECK(xwc_text_width2("Hello") > xwc_text_width("Hello"),
              "width2(%d) > width(%d)", xwc_text_width2("Hello"),
              xwc_text_width("Hello"));
    XWT_CHECK(xwc_text_width("") == 0 && xwc_text_width2("") == 0,
              "empty strings are zero-width");

    /* rendering: text2 lands pixels inside a 40px-tall row */
    uint32_t pix[80 * 40];
    memset(pix, 0, sizeof(pix));
    xwc_draw_text2(pix, 80, 80, 40, 4, 4, "Ag", 0xffffffff);
    int lit = 0;
    for (size_t i = 0; i < 80 * 40; i++)
        if ((pix[i] >> 24) > 0x80)
            lit++;
    XWT_CHECK(lit > 20, "font2 glyphs rasterized (%d px)", lit);

    /* vline helper */
    memset(pix, 0, sizeof(pix));
    xwc_draw_vline(pix, 80, 80, 40, 10, 5, 12, 0xff00ff00);
    XWT_CHECK(pix[5 * 80 + 10] == 0xff00ff00, "vline fills its column");
}

/* UTF-8 rendering: accented Latin glyphs must rasterize (the old
 * ASCII-only font drew them as invisible blank gaps — application
 * names appeared to be missing letters), the ellipsis must exist, and
 * uncovered codepoints render a visible fallback box instead of
 * vanishing. Truncation must never split a multibyte sequence. */
static int lit_pixels(const uint32_t *pix, size_t n) {
    int lit = 0;
    for (size_t i = 0; i < n; i++)
        if ((pix[i] >> 24) > 0x80)
            lit++;
    return lit;
}

static void test_utf8_render(struct xwt_ctx *t) {
    (void)t;
    /* Latin coverage: these codepoints are inside the rasterized
     * ranges and MUST draw pixels */
    static const struct {
        const char *label;
        const char *text;
    } cases[] = {
        {"e-acute", "Caf\xc3\xa9"},
        {"u-umlaut", "Gr\xc3\xb6\xc3\x9f" "e"},
        {"eszett", "Stra\xc3\x9f" "e"},
        {"c-caron", "\xc4\x8d\xc3\xa8\xc5\xa1"},
        {"euro", "\xe2\x82\xac"},
        {"ellipsis", "\xe2\x80\xa6"},
        {"en-dash", "\xe2\x80\x93"},
        {"degree", "45\xc2\xb0"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t pix[200 * 40];
        memset(pix, 0, sizeof(pix));
        xwc_draw_text(pix, 200, 200, 40, 4, 4, cases[i].text, 0xffffffff);
        int lit = lit_pixels(pix, 200 * 40);
        XWT_CHECK(lit > 4, "%s rasterized (%d px)", cases[i].label, lit);
    }

    /* a codepoint outside the coverage (CJK): a visible fallback box,
     * not an invisible blank gap */
    uint32_t pix[100 * 40];
    memset(pix, 0, sizeof(pix));
    xwc_draw_text(pix, 100, 100, 40, 4, 4, "\xe6\xbc\xa2", 0xffffffff);
    int cjk_lit = lit_pixels(pix, 100 * 40);
    XWT_CHECK(cjk_lit >= 8, "uncovered codepoint draws a fallback box (%d)",
              cjk_lit);

    /* widths are codepoint-aware: multibyte characters are not
     * byte-counted */
    XWT_CHECK(xwc_text_width("\xc3\xa9") == xwc_text_width("\xc3\xa8"),
              "two accented e's share one advance");
    XWT_CHECK(xwc_text_width("\xe2\x80\xa6") > 0, "ellipsis has width");

    /* malformed UTF-8 (a sequence cut mid-character) never draws
     * garbage and never reads past the terminator */
    memset(pix, 0, sizeof(pix));
    xwc_draw_text(pix, 100, 100, 40, 4, 4, "abc\xc3", 0xffffffff);
    XWT_CHECK(lit_pixels(pix, 100 * 40) > 8, "prefix before a cut sequence "
              "still renders");
}

static void test_xpm_direct(struct xwt_ctx *t) {
    (void)t;
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-%d", (int)getpid());
    env_snapshot();
    t_mkdir(t_dir);
    env_redirect(t_dir, t_dir, t_dir); /* hermetic: nothing real */
    char path[300];
    snprintf(path, sizeof(path), "%s/red4.xpm", t_dir);
    write_file(path, XPM_RED4);

    const struct xwc_icon *ic = xwc_icon_get(path, 24);
    XWT_CHECK(ic && ic->pix, "absolute-path XPM decoded");
    if (ic) {
        /* 4x4 upscaled to 24x24 (gap 20 > 24/4) */
        XWT_CHECK(ic->w == 24 && ic->h == 24, "upscaled to 24 (%dx%d)",
                  ic->w, ic->h);
        /* source (0,0) is 'X' (red): upscale maps it to dst 0..5 */
        uint32_t p = ic->pix[0 * 24 + 0];
        XWT_CHECK(p == 0xffff0000, "upscaled red pixel (%08x)", p);
        /* source (1,1) is '.' (transparent): maps to dst 6..11 */
        XWT_CHECK((ic->pix[6 * 24 + 6] >> 24) == 0,
                  "transparent stays 0 (%08x)", ic->pix[6 * 24 + 6]);
    }
    env_restore();
    rmdir(path);
}

static void test_png_direct(struct xwt_ctx *t) {
    (void)t;
    const char *p = asset("red16.png");
    XWT_ASSERT(p);
    if (!xwc_icon_png()) {
        XWT_SKIP("png not compiled in: decode test");
        return;
    }
    env_snapshot();
    env_redirect("/tmp", "/tmp", "/tmp");
    const struct xwc_icon *ic = xwc_icon_get(p, 16);
    XWT_CHECK(ic && ic->pix, "PNG decoded");
    if (ic) {
        XWT_CHECK(ic->w == 16 && ic->h == 16, "16x16 loaded as-is");
        /* right half is opaque red */
        uint32_t q = ic->pix[8 * 16 + 12];
        XWT_CHECK(q == 0xffff0000, "opaque red right half (%08x)", q);
        /* left half carries the alpha gradient (x=0 -> alpha 32) */
        uint32_t a = ic->pix[8 * 16 + 0];
        XWT_CHECK((a >> 24) == 31 || (a >> 24) == 32,
                  "alpha gradient preserved (%08x)", a);
    }
    /* downscale: 32x32 blue requested at 16 -> 16x16 box average */
    const char *p2 = asset("blue32.png");
    const struct xwc_icon *ic2 = xwc_icon_get(p2, 16);
    XWT_CHECK(ic2 && ic2->w == 16 && ic2->h == 16, "32x32 downscaled to 16");
    if (ic2) {
        uint32_t c = ic2->pix[8 * 16 + 8];
        XWT_CHECK(c == 0xff0050ff, "downscale keeps solid color (%08x)", c);
    }
    env_restore();
}

static void test_theme_lookup(struct xwt_ctx *t) {
    (void)t;
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-t-%d", (int)getpid());
    char user[300], sys[300];
    snprintf(user, sizeof(user), "%s/user", t_dir);
    snprintf(sys, sizeof(sys), "%s/sys", t_dir);
    mkdirs("%s/user/icons/hicolor/32x32/apps", t_dir);
    mkdirs("%s/sys/icons/hicolor/48x48/apps", t_dir);
    char u32[400], s48[400];
    snprintf(u32, sizeof(u32), "%s/user/icons/hicolor/32x32/apps/app.xpm",
             t_dir);
    snprintf(s48, sizeof(s48), "%s/sys/icons/hicolor/48x48/apps/app.xpm",
             t_dir);
    write_file(u32, XPM_RED4);
    write_file(s48, XPM_BLUE4);

    env_snapshot();
    env_redirect(t_dir, user, sys);

    /* size selection: request 24 -> the 32x32 entry (smallest >= 24)
     * wins over the 48x48 one, and downscales to 24x24 */
    const struct xwc_icon *ic = xwc_icon_get("app", 24);
    XWT_CHECK(ic && ic->pix, "themed icon found");
    if (ic) {
        XWT_CHECK(ic->w == 24 && ic->h == 24, "rendered at 24 (%dx%d)",
                  ic->w, ic->h);
        XWT_CHECK(ic->pix[0] == 0xffff0000, "32px (red) file chosen");
    }

    /* request 40: 48 >= 40 -> blue, downscaled to 40 */
    const struct xwc_icon *ic48 = xwc_icon_get("app", 40);
    XWT_CHECK(ic48 && ic48->w == 40 && ic48->h == 40, "48 entry at 40");
    if (ic48)
        XWT_CHECK(ic48->pix[20 * 40 + 20] == 0xff0000ff,
                  "48px (blue) file chosen");

    /* cache: the same key returns the identical object */
    const struct xwc_icon *again = xwc_icon_get("app", 24);
    XWT_CHECK(again == ic, "cache returns the same surface");

    /* misses are clean NULLs */
    XWT_CHECK(xwc_icon_get("definitely-not-there", 24) == NULL,
              "missing icon -> NULL");

    env_restore();
}

static void test_theme_priority(struct xwt_ctx *t) {
    (void)t;
    /* the user dir overrides the system dir for the same name */
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-p-%d", (int)getpid());
    char user[300], sys[300];
    snprintf(user, sizeof(user), "%s/user", t_dir);
    snprintf(sys, sizeof(sys), "%s/sys", t_dir);
    mkdirs("%s/user/icons/hicolor/48x48/apps", t_dir);
    mkdirs("%s/sys/icons/hicolor/48x48/apps", t_dir);
    char up[400], sp[400];
    snprintf(up, sizeof(up), "%s/user/icons/hicolor/48x48/apps/dup.xpm",
             t_dir);
    snprintf(sp, sizeof(sp), "%s/sys/icons/hicolor/48x48/apps/dup.xpm",
             t_dir);
    write_file(up, XPM_RED4);
    write_file(sp, XPM_BLUE4);

    env_snapshot();
    env_redirect(t_dir, user, sys);
    const struct xwc_icon *ic = xwc_icon_get("dup", 48);
    XWT_CHECK(ic && ic->pix, "icon resolved");
    if (ic)
        XWT_CHECK(ic->pix[24 * 48 + 24] == 0xffff0000,
                  "user file wins over system (%08x)",
                  ic->pix[24 * 48 + 24]);
    env_restore();
}

static void test_icon_blit(struct xwt_ctx *t) {
    (void)t;
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-b-%d", (int)getpid());
    env_snapshot();
    t_mkdir(t_dir);
    env_redirect(t_dir, t_dir, t_dir);
    char path[300];
    snprintf(path, sizeof(path), "%s/blue4.xpm", t_dir);
    write_file(path, XPM_BLUE4);

    const struct xwc_icon *ic = xwc_icon_get(path, 32);
    XWT_ASSERT(ic && ic->pix);
    uint32_t pix[64 * 40];
    memset(pix, 0x33, sizeof(pix)); /* 0x33333333-ish background */
    /* blit into a 32x32 cell at (8,4) */
    xwc_draw_icon(pix, 64, 64, 40, 8, 4, ic, 32);
    XWT_CHECK(pix[20 * 64 + 24] == 0xff0000ff,
              "icon blitted at cell center (%08x)", pix[20 * 64 + 24]);
    /* outside the cell: untouched background */
    XWT_CHECK(pix[20 * 64 + 60] == 0x33333333, "outside untouched");
    env_restore();
}

/* theme inheritance: the configured theme misses the icon, its
 * Inherit= parent has it — the chain must resolve it */
static void test_theme_inherit(struct xwt_ctx *t) {
    (void)t;
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-i-%d", (int)getpid());
    char user[300];
    snprintf(user, sizeof(user), "%s/user", t_dir);
    /* "mytheme" (empty of the icon) inherits "parenttheme" which has it */
    mkdirs("%s/user/icons/mytheme/48x48/apps", t_dir);
    mkdirs("%s/user/icons/parenttheme/48x48/apps", t_dir);
    mkdirs("%s/user/icons/hicolor/48x48/apps", t_dir);
    char idx[400], pp[400];
    snprintf(idx, sizeof(idx), "%s/user/icons/mytheme/index.theme", t_dir);
    write_file(idx, "[Icon Theme]\nName=My Theme\nInherit=parenttheme\n");
    snprintf(pp, sizeof(pp),
             "%s/user/icons/parenttheme/48x48/apps/inherited.xpm", t_dir);
    write_file(pp, XPM_RED4);
    /* hicolor carries a DIFFERENT icon under the same name: the chain
     * parent must win over the hicolor fallback */
    char hp[400];
    snprintf(hp, sizeof(hp),
             "%s/user/icons/hicolor/48x48/apps/inherited.xpm", t_dir);
    write_file(hp, XPM_BLUE4);

    env_snapshot();
    env_redirect(t_dir, user, NULL);
    setenv("XW_ICON_THEME", "mytheme", 1);

    const struct xwc_icon *ic = xwc_icon_get("inherited", 48);
    XWT_CHECK(ic && ic->pix, "icon found through the Inherit= chain");
    if (ic)
        XWT_CHECK(ic->pix[24 * 48 + 24] == 0xffff0000,
                  "parent theme wins over hicolor (%08x)",
                  ic->pix[24 * 48 + 24]);

    unsetenv("XW_ICON_THEME");
    env_restore();
}

/* the desktop's configured theme: gtk-3.0 settings.ini is honored */
static void test_theme_gtk_discovery(struct xwt_ctx *t) {
    (void)t;
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-g-%d", (int)getpid());
    char home[300];
    snprintf(home, sizeof(home), "%s/home", t_dir);
    char data[300];
    snprintf(data, sizeof(data), "%s/data", t_dir);
    mkdirs("%s/home/.config/gtk-3.0", t_dir);
    mkdirs("%s/data/icons/fancytheme/48x48/apps", t_dir);
    char ini[400], icp[400];
    snprintf(ini, sizeof(ini), "%s/home/.config/gtk-3.0/settings.ini",
             t_dir);
    write_file(ini, "[Settings]\n gtk-icon-theme-name=fancytheme\n");
    snprintf(icp, sizeof(icp),
             "%s/data/icons/fancytheme/48x48/apps/fancy.xpm", t_dir);
    write_file(icp, XPM_BLUE4);

    env_snapshot();
    env_redirect(home, data, data);
    unsetenv("XW_ICON_THEME");

    const struct xwc_icon *ic = xwc_icon_get("fancy", 48);
    XWT_CHECK(ic && ic->pix, "gtk-3.0 icon theme discovered");
    env_restore();
}

/* the XFCE xfconf IconThemeName property is honored */
static void test_theme_xfce_discovery(struct xwt_ctx *t) {
    (void)t;
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-x-%d", (int)getpid());
    char home[300], data[300];
    snprintf(home, sizeof(home), "%s/home", t_dir);
    snprintf(data, sizeof(data), "%s/data", t_dir);
    mkdirs("%s/home/.config/xfce4/xfconf/xfce-perchannel-xml", t_dir);
    mkdirs("%s/data/icons/xfctheme/48x48/apps", t_dir);
    char xml[400], icp[400];
    snprintf(xml, sizeof(xml),
             "%s/home/.config/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml",
             t_dir);
    write_file(xml,
               "<channel name=\"xsettings\">\n"
               "  <property name=\"IconThemeName\" type=\"string\" "
               "value=\"xfctheme\"/>\n"
               "</channel>\n");
    snprintf(icp, sizeof(icp),
             "%s/data/icons/xfctheme/48x48/apps/xfc.xpm", t_dir);
    write_file(icp, XPM_RED4);

    env_snapshot();
    env_redirect(home, data, data);
    unsetenv("XW_ICON_THEME");

    const struct xwc_icon *ic = xwc_icon_get("xfc", 48);
    XWT_CHECK(ic && ic->pix, "xfconf IconThemeName discovered");
    env_restore();
}

/* Icon= values carrying an extension ("foo.png") still resolve */
static void test_theme_ext_strip(struct xwt_ctx *t) {
    (void)t;
    snprintf(t_dir, sizeof(t_dir), "/tmp/xwt-icons-e-%d", (int)getpid());
    char user[300];
    snprintf(user, sizeof(user), "%s/user", t_dir);
    mkdirs("%s/user/icons/hicolor/48x48/apps", t_dir);
    char p[400];
    snprintf(p, sizeof(p), "%s/user/icons/hicolor/48x48/apps/extapp.xpm",
             t_dir);
    write_file(p, XPM_RED4);

    env_snapshot();
    env_redirect(t_dir, user, NULL);
    unsetenv("XW_ICON_THEME");

    const struct xwc_icon *ic = xwc_icon_get("extapp.png", 48);
    XWT_CHECK(ic && ic->pix, "extension-stripped lookup resolves");
    env_restore();
}

static const struct xwt_test tests[] = {
    {"client-font2", test_font2},
    {"client-icon-xpm-direct", test_xpm_direct},
    {"client-icon-png-direct", test_png_direct},
    {"client-font-utf8", test_utf8_render},
    {"client-icon-theme-lookup", test_theme_lookup},
    {"client-icon-theme-inherit", test_theme_inherit},
    {"client-icon-theme-gtk", test_theme_gtk_discovery},
    {"client-icon-theme-xfce", test_theme_xfce_discovery},
    {"client-icon-theme-ext-strip", test_theme_ext_strip},
    {"client-icon-theme-priority", test_theme_priority},
    {"client-icon-blit", test_icon_blit},
};

__attribute__((constructor)) static void register_tests(void) {
    xwt_register(tests, sizeof(tests) / sizeof(tests[0]));
}
