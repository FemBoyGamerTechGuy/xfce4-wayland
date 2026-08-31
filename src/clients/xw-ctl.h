/* xw-ctl.h — session control-socket client helper shared by the
 * graphical clients (xw-exit, xw-panel): one request, one reply line.
 *
 * The protocol is documented in ARCHITECTURE.md (private unix line
 * protocol on $XDG_RUNTIME_DIR/xw-session.sock).
 */
#ifndef XW_CTL_H
#define XW_CTL_H

#include <stdbool.h>
#include <stddef.h>

/* Send one command to the session manager; reply (first line, newline
 * stripped) lands in reply.  Returns true iff the manager answered
 * "ok".  reply is always NUL-terminated and carries the error text on
 * failure ("no session manager (...)" etc.) so callers can log it. */
bool xw_ctl_send(const char *cmd, char *reply, size_t reply_len);

#endif /* XW_CTL_H */
