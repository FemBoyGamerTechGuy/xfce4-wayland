# TODO

Persistent task list (mirrors the live development tracker). Status:
[ ] open, [x] done, [~] partial (gaps noted in ROADMAP.md).

## Session 1 scope
- [x] M0 repository + licensing + build + docs skeleton
- [x] M1 libxw core: server bootstrap, headless backend, outputs,
      shm/surfaces, pixman renderer, compositor binary
- [x] M2 xdg-shell + window manager (states, workspaces, focus,
      stacking, move/resize, snap, rules)
- [x] M3 seat + xkb + shortcut engine + defaults + persistence
- [x] M4 layer-shell / xdg-activation / foreign-toplevel / ext-workspace
- [x] M5 clipboard + drag&drop (core data-device)
- [x] M6 xw-session (autostart, supervision, ctl socket, power)
- [x] M7 xw-exit graphical session-exit dialog
- [ ] M8 xw-panel v0 (next)
- [~] M9 automated test suite + regression policy (16 in-process +
      18 process checks, ASAN-clean; fuzzing/CI pending)
- [ ] M10 docs/WORKLOG polish, git milestones, release archive

## Backlog highlights (tracked in detail in ROADMAP.md)
- [ ] key repeat; touch input; per-seat layout switching
- [ ] ext-session-lock + idle-notify (screen lock, screensaver)
- [ ] ext-image-copy-capture screenshot tool
- [ ] wlr-output-management display settings
- [ ] panel plugin API; notification daemon; desktop icons; wallpaper
- [ ] application finder
- [ ] XWayland optional compatibility
- [ ] DRM/KMS + nested backends; dmabuf; GL renderer path
- [ ] session save/restore; PAM unlock
- [ ] popup grab/keyboard dismissal; drag icons; DnD action negotiation
- [ ] session restart (re-exec) automated test
- [ ] fuzzing + sanitizer CI
