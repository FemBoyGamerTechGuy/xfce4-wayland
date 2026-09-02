#!/bin/sh
# One-shot credential helper for pushing to origin without persisting
# any secret to disk. git calls this with the prompt string ("Username
# for 'https://github.com': ..." / "Password for ..."); we answer both
# with the token supplied via the XW_PUSH_TOKEN environment variable.
# The token itself is NEVER written to this file (or anywhere else) —
# it lives only in the environment of the single git command.
#
# Usage:
#   XW_PUSH_TOKEN='github_pat_...' GIT_TERMINAL_PROMPT=0 \
#   GIT_ASKPASS=$(pwd)/scripts/git-askpass-env.sh \
#   git -c credential.helper= push origin main
#
# The `-c credential.helper=` override is important: it disables any
# configured store/cache helper for that invocation, guaranteeing the
# token is not silently saved to ~/.git-credentials.
echo "${XW_PUSH_TOKEN:-}"
