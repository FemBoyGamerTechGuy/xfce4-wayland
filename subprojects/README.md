# subprojects/

Home for third-party or XFCE-derived components that are kept as
separately maintained trees with their own licenses and provenance.

Rules:

1. Nothing is vendored here by default. A component is only added after
   a reuse/adapt/rewrite/reimplement/replace decision is recorded in
   THIRD-PARTY-LICENSES.md (provenance table) and WORKLOG.md.
2. Anything under `subprojects/` keeps its upstream license and
   copyright headers untouched; it is never covered by the repository's
   proprietary LICENSE.
3. Build integration must keep the boundary visible: subproject code
   never includes original headers that pull it into the proprietary
   tree, and original code links to subprojects only through documented
   interfaces.
4. Copyleft components (GPL) may live here but may only be used via
   process boundaries (exec), not linked into proprietary binaries.

Current contents: none. As of this writing the desktop is implemented
entirely from original code (behavioral re-implementation of XFCE
semantics; no XFCE source incorporated — see THIRD-PARTY-LICENSES.md
section 3).
