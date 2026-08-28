#!/bin/bash
# Apply (or verify) the licence header on every first-party source file.
#
# release-plan.md §5 splits the tree in two, and this script is the only place that split is
# mechanical rather than documentary:
#
#   MPL-2.0     src/, tests/, tools/  — the client. File-level copyleft: modifications to these
#                                       files come back, and the files can still be combined
#                                       into a larger work.
#   Apache-2.0  server/, bench/       — the reference server and the protocol's own side. A spec
#                                       and a reference implementation want to spread without
#                                       anyone reading a licence first.
#
# docs/protocol.md carries its own notice in prose; it is not source and is not touched here.
#
#   tools/apply-license-headers.sh          # add missing headers
#   tools/apply-license-headers.sh --check  # exit 1 if any file lacks one
#
# Idempotent: a file that already carries either marker is left alone.
set -e
cd "$(dirname "$0")/.."

CHECK=0
[ "$1" = "--check" ] && CHECK=1

MPL_MARKER="subject to the terms of the Mozilla Public"
APACHE_MARKER="Licensed under the Apache License"

mpl_slash() {
    cat <<'EOF'
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
EOF
}

mpl_hash() {
    cat <<'EOF'
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
EOF
}

apache_hash() {
    cat <<'EOF'
# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
EOF
}

MISSING=0

# $1 file, $2 emitter function name, $3 marker
process() {
    local f="$1" emit="$2" marker="$3"
    if grep -q "$marker" "$f"; then
        return
    fi
    if [ "$CHECK" -eq 1 ]; then
        echo "missing header: $f"
        MISSING=1
        return
    fi
    local tmp
    tmp=$(mktemp)
    if head -1 "$f" | grep -q '^#!'; then
        # Keep the shebang on line 1.
        { head -1 "$f"; "$emit"; tail -n +2 "$f"; } > "$tmp"
    else
        { "$emit"; cat "$f"; } > "$tmp"
    fi
    mv "$tmp" "$f"
    echo "added header: $f"
}

# ─── MPL-2.0: the client ────────────────────────────────────────────────────────────────────
while IFS= read -r f; do
    process "$f" mpl_slash "$MPL_MARKER"
done < <(find src tests -type f \( -name '*.cpp' -o -name '*.h' \) | sort)

while IFS= read -r f; do
    process "$f" mpl_slash "$MPL_MARKER"
done < <(find tools -type f \( -name '*.cpp' -o -name '*.h' \) | sort)

while IFS= read -r f; do
    process "$f" mpl_hash "$MPL_MARKER"
done < <(find tools -type f -name '*.sh' | sort)

# ─── Apache-2.0: the reference server ───────────────────────────────────────────────────────
while IFS= read -r f; do
    process "$f" apache_hash "$APACHE_MARKER"
done < <(find server bench -type f -name '*.py' 2>/dev/null | sort)

if [ "$MISSING" -eq 1 ]; then
    echo
    echo "Run tools/apply-license-headers.sh (no arguments) to add them."
    exit 1
fi
