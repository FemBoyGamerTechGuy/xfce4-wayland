/* panel-apps.h — the XDG application database of the xw panel.
 *
 * Discovery, parsing, filtering, categorization and launch-command
 * construction for .desktop files, per the freedesktop Desktop Entry
 * Specification. Client-side only: the panel links no compositor or
 * session code (see subprojects/README.md).
 *
 * A database is built by scanning $XDG_DATA_HOME/applications (default
 * ~/.local/share/applications) first, then every $XDG_DATA_DIRS entry
 * (default /usr/local/share:/usr/share); a user entry shadows a
 * system entry with the same desktop-file basename, like XDG autostart.
 * Entries are filtered by the spec's visibility rules (Type=Application,
 * NoDisplay, Hidden, OnlyShowIn/NotShowIn, TryExec) and grouped into
 * the XFCE-style main categories. Everything is sorted by display name.
 */
#ifndef PANEL_APPS_H
#define PANEL_APPS_H

#include <stdbool.h>
#include <stddef.h>

/* the main categories, XFCE applications-menu style ("Internet" for
 * Network, "Multimedia" for AudioVideo — the names desktop files
 * actually use; see cat_name()) */
enum {
    XWAPP_CAT_FAVORITES = 0, /* synthesized from the favorites list */
    XWAPP_CAT_ALL,
    XWAPP_CAT_ACCESSORIES,  /* Utility (not Settings) */
    XWAPP_CAT_DEVELOPMENT,
    XWAPP_CAT_EDUCATION,
    XWAPP_CAT_GAMES,
    XWAPP_CAT_GRAPHICS,
    XWAPP_CAT_MULTIMEDIA,   /* AudioVideo */
    XWAPP_CAT_NETWORK,      /* Network / Internet */
    XWAPP_CAT_OFFICE,
    XWAPP_CAT_SCIENCE,
    XWAPP_CAT_SETTINGS,
    XWAPP_CAT_SYSTEM,
    XWAPP_CAT_TERMINAL,
    XWAPP_CAT_OTHER,
    XWAPP_CAT_COUNT
};

#define XWAPP_NAME_MAX 128
#define XWAPP_COMMENT_MAX 256
#define XWAPP_EXEC_MAX 256
#define XWAPP_ID_MAX 160
#define XWAPP_PATH_MAX 512

struct xwapp {
    char desktop_id[XWAPP_ID_MAX]; /* basename, no .desktop extension */
    char name[XWAPP_NAME_MAX];     /* localized display name */
    char generic[XWAPP_NAME_MAX];  /* GenericName */
    char comment[XWAPP_COMMENT_MAX];
    char exec[XWAPP_EXEC_MAX];     /* raw Exec line */
    char icon[XWAPP_NAME_MAX];     /* Icon name or path ("" = none) */
    char categories[XWAPP_COMMENT_MAX]; /* raw Categories */
    char path[XWAPP_PATH_MAX];     /* source .desktop file */
    bool terminal;                 /* Terminal=true */
    int cat;                       /* XWAPP_CAT_* (never FAVORITES/ALL) */
    char sort_key[XWAPP_NAME_MAX]; /* casefolded name for ordering */
};

#define XWAPP_MAX 1600  /* installed-apps cap (real boxes: 200..1000) */
#define XWAPP_MAX_ARGS 32
#define XWAPP_ARG_MAX 200

struct xwapp_db {
    struct xwapp apps[XWAPP_MAX];
    int n_apps;
    /* per-category index arrays into apps[] (sorted by name) */
    int cat_idx[XWAPP_CAT_COUNT][XWAPP_MAX];
    int cat_n[XWAPP_CAT_COUNT];
    /* favorites: desktop ids from the config; resolved at query time */
    char favorites[64][XWAPP_ID_MAX];
    int n_favorites;
};

/* build the database by scanning the XDG directories (reuses the
 * caller's struct; rescans are cheap when directories are unchanged
 * thanks to the mtime cache). Returns 0 on success. */
int xwapp_db_scan(struct xwapp_db *db);

/* favorites management: ids that are set here AND present in the
 * database appear in the FAVORITES category. Unknown ids are kept
 * (the file may list apps not installed yet). */
void xwapp_db_set_favorites(struct xwapp_db *db, const char *const *ids,
                            int n);
int xwapp_db_favorites(const struct xwapp_db *db,
                        const char *out[][XWAPP_ID_MAX]);

int xwapp_db_count(const struct xwapp_db *db);
const struct xwapp *xwapp_at(const struct xwapp_db *db, int i);
const struct xwapp *xwapp_by_id(const struct xwapp_db *db,
                                const char *desktop_id);

/* category listing (index order = name-sorted) */
int xwapp_cat_count(const struct xwapp_db *db, int cat);
const struct xwapp *xwapp_cat_at(const struct xwapp_db *db, int cat, int i);

/* category display name ("Accessories", "Internet", ...) */
const char *xwapp_cat_name(int cat);
/* themed icon name for a category ("applications-internet", ...) */
const char *xwapp_cat_icon(int cat);

/* case-insensitive substring search over name, generic name, comment
 * and desktop id. Fills out[] with app indices (cap), returns the
 * total number of matches. */
int xwapp_search(const struct xwapp_db *db, const char *needle, int *out,
                 int cap);

/* Exec parsing: split `exec` into argv honoring the desktop-entry
 * quoting rules (backslash escapes, double quotes, reserved symbols
 * outside quotes), applying the field codes:
 *   %% -> literal %, %f %F %u %U -> dropped (no file context),
 *   %i -> --icon <icon>, %c -> the (localized) name,
 *   %k -> the .desktop file path (as a file:// URI when non-local,
 *   plain path otherwise; NULL/"" drops it like the file codes),
 *   deprecated codes (%d %D %n %N %v %m) -> dropped per the spec.
 * Returns the argument count, or -1 on malformed quoting. */
int xwapp_exec_argv(const char *exec, const char *icon, const char *name,
                    const char *desktop_path, char args[][XWAPP_ARG_MAX],
                    int max);

/* the full launch vector for an app: xwapp_exec_argv plus the
 * terminal wrapper when Terminal=true (see terminal_strategy below).
 * Returns the count, or -1 when the app cannot run. */
int xwapp_launch_argv(const struct xwapp *a, char args[][XWAPP_ARG_MAX],
                      int max, char *err, size_t err_n);

/* serialize an argv into the single line the session's ctl "run"
 * understands (sh -c): every argument is shell-quoted only when it
 * needs to be. Writes to out, returns false when it does not fit. */
bool xwapp_argv_to_shell(const char args[][XWAPP_ARG_MAX], int n, char *out,
                         size_t out_n);

/* terminal resolution, exposed for tests and the fallback launcher:
 * $XW_TERMINAL wins when it names a runnable program; otherwise the
 * first available common terminal. `style` picks the execution
 * convention used to host Terminal=true commands. */
enum {
    XWAPP_TERM_E_REST,   /* TERM -e CMD ARGS... (xterm, st, konsole..) */
    XWAPP_TERM_DASHDASH, /* TERM -- CMD ARGS... (gnome-terminal, wezterm) */
    XWAPP_TERM_POSITIONAL, /* TERM CMD ARGS... (kitty, foot) */
    XWAPP_TERM_X,        /* TERM -x CMD ARGS... (xfce4-terminal, terminator) */
};
bool xwapp_resolve_terminal(char *out, size_t n, int *style);
bool xwapp_path_has(const char *name);

#endif /* PANEL_APPS_H */
