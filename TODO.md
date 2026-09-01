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
- [x] M8 xw-panel v0 (bar + workspace switcher + tasklist + launcher
      + clock + exit button, session-ctl `exit-dialog`/`run` wiring)
- [~] M9 automated test suite + regression policy (32 in-process +
      61 process checks, 3-level strategy, ASAN-clean; fuzzing/CI
      pending)
- [x] M10 docs/WORKLOG polish, git milestones, distro-agnostic build
      guide, install targets, zero-root verification

## Phase 2-3 (current session)
- [x] nested Wayland + X11 backends (in-process + process tested)
- [x] build system: feature toggles, profiles, dep diagnostics,
      install/uninstall, config summary, session .desktop
- [x] real-input backend (libinput): udev seat + path modes,
      translation pipeline, `-I/--input`, white-box Level-1 tests
- [x] key repeat (protocol-correct) + X11 synthetic-repeat filtering
- [x] logind/elogind power backend: probing, reasons, no-shell exec,
      exit-dialog availability UX, power-status ctl
- [x] keysym canonicalization (ISO_Left_Tab/Sys_Req/KP_Enter) +
      full default-shortcut coverage test
- [x] session passes user config dir to the compositor

## Backlog highlights (tracked in detail in ROADMAP.md)
- [ ] touch input; per-seat layout switching
- [ ] DRM/KMS backend (Phase 4): device discovery, CRTCs, planes,
      atomic modesetting, multi-monitor, hotplug; logind DRM master
- [x] ext-session-lock + idle-notify (screen lock, screensaver):
      server protocol + security gates, xw-lock client, xwc_lock/xwc_idle
- [~] PAM unlock backend (current authenticator: local passphrase file
      via $XW_LOCK_PASSPHRASE_FILE or ~/.config/xfce4-wayland/lock-pass)
- [ ] ext-image-copy-capture screenshot tool
- [ ] wlr-output-management display settings
- [ ] panel plugin API; notification daemon; desktop icons; wallpaper
- [ ] application finder
- [ ] XWayland optional compatibility
- [ ] dmabuf; GL renderer path
- [ ] session save/restore; PAM unlock
- [ ] popup grab/keyboard dismissal; drag icons; DnD action negotiation
- [ ] session restart (re-exec) automated test
- [ ] fuzzing + sanitizer CI
