/* panel-config.c — the panel's INI reader and defaults.
 *
 * Deliberately a panel-local copy (the compositor's INI parser lives
 * in libxw, which the panel must not link): ~150 lines of plain C,
 * the same [section] key=value shape, # and ; comments. Defaults are
 * complete: a machine without any config file gets a sane XFCE-ish
 * bar. */
#include "panel.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void panel_config_defaults(struct panel_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->height = 0; /* auto */
    cfg->bottom = false;
    snprintf(cfg->clock_format, sizeof(cfg->clock_format),
             "%%a %%d %%b %%H:%%M");
    cfg->clock_seconds = false;
    cfg->tasklist_style = 0; /* icons + text */
    cfg->menu_icons = true;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = 0;
    return s;
}

static void strip_comment(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == '#' || *p == ';') {
            *p = 0;
            return;
        }
    }
}

static bool parse_bool(const char *v, bool dflt) {
    if (!v || !*v)
        return dflt;
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
           strcmp(v, "yes") == 0 || strcmp(v, "on") == 0;
}

static int parse_int(const char *v, int dflt) {
    if (!v || !*v)
        return dflt;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end && *end == 0)
        return (int)n;
    return dflt;
}

bool panel_config_line(struct panel_config *cfg, const char *line_in) {
    char line[1024];
    snprintf(line, sizeof(line), "%.1000s", line_in);
    strip_comment(line);
    char *s = trim(line);
    if (!*s)
        return true;

    static char section[32];
    if (*s == '[') {
        char *close = strchr(s, ']');
        if (!close)
            return false;
        *close = 0;
        snprintf(section, sizeof(section), "%.28s", trim(s + 1));
        return true;
    }
    char *eq = strchr(s, '=');
    if (!eq)
        return false;
    *eq = 0;
    char *key = trim(s);
    char *val = trim(eq + 1);
    if (!*key)
        return false;

    if (strcmp(section, "panel") == 0) {
        if (strcmp(key, "height") == 0) {
            cfg->height = strcmp(val, "auto") == 0 ? 0 : parse_int(val, 0);
            if (cfg->height < 24 || cfg->height > 200)
                cfg->height = 0;
        } else if (strcmp(key, "position") == 0) {
            cfg->bottom = strcmp(val, "bottom") == 0;
        } else if (strcmp(key, "launchers") == 0) {
            snprintf(cfg->launchers, sizeof(cfg->launchers), "%.500s", val);
        } else if (strcmp(key, "favorites") == 0) {
            snprintf(cfg->favorites, sizeof(cfg->favorites), "%.500s", val);
        } else if (strcmp(key, "icon-theme") == 0) {
            snprintf(cfg->icon_theme, sizeof(cfg->icon_theme), "%.60s", val);
        }
    } else if (strcmp(section, "clock") == 0) {
        if (strcmp(key, "format") == 0) {
            snprintf(cfg->clock_format, sizeof(cfg->clock_format), "%.60s",
                     val);
        } else if (strcmp(key, "seconds") == 0) {
            cfg->clock_seconds = parse_bool(val, false);
        }
    } else if (strcmp(section, "tasklist") == 0) {
        if (strcmp(key, "style") == 0) {
            if (strcmp(val, "icons") == 0)
                cfg->tasklist_style = 1;
            else if (strcmp(val, "text") == 0)
                cfg->tasklist_style = 2;
            else
                cfg->tasklist_style = 0;
        }
    } else if (strcmp(section, "menu") == 0) {
        if (strcmp(key, "icons") == 0)
            cfg->menu_icons = parse_bool(val, true);
    }
    return true;
}

void panel_config_load(struct panel_config *cfg) {
    panel_config_defaults(cfg);

    /* $XDG_CONFIG_HOME/xw-panel/panel.conf (default ~/.config), with
     * $XW_PANEL_CONF as an explicit override for tests/embedded use */
    char path[512];
    const char *env = getenv("XW_PANEL_CONF");
    if (env && *env) {
        snprintf(path, sizeof(path), "%.480s", env);
    } else {
        const char *xch = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        if (xch && *xch)
            snprintf(path, sizeof(path), "%.240s/xw-panel/panel.conf", xch);
        else if (home && *home)
            snprintf(path, sizeof(path),
                     "%.220s/.config/xw-panel/panel.conf", home);
        else
            return; /* no HOME: defaults only */
    }
    FILE *f = fopen(path, "r");
    if (!f)
        return; /* no config file: defaults are complete */
    char line[1024];
    while (fgets(line, sizeof(line), f))
        panel_config_line(cfg, line);
    fclose(f);

    /* the icon theme choice is an env-level input for the icon cache;
     * only set it when the file names one and the env does not */
    if (cfg->icon_theme[0] && !getenv("XW_ICON_THEME"))
        setenv("XW_ICON_THEME", cfg->icon_theme, 1);
}
