#!/usr/bin/env bash
# Copy the Catalyst UI kit into src/components/catalyst/.
#
# Catalyst is part of Tailwind Plus and licensed, so its sources are not in this
# (public) repository: they come from your own copy, and the directory they land
# in is gitignored. catalyst-overrides/ holds our own adapters (link.tsx, for
# next/link), copied over the kit's versions.
#
#   CATALYST_DIR  the kit's typescript/ directory
#                 (default ~/management-sensors/frontend/private/catalyst-ui-kit/typescript)
#
# Without the kit, scripts/fetch-webroot.sh downloads an already built panel.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${CATALYST_DIR:-${HOME}/management-sensors/frontend/private/catalyst-ui-kit/typescript}"
DEST="${ROOT}/src/components/catalyst"

if [[ ! -f "${SRC}/button.tsx" ]]; then
    if [[ -f "${DEST}/button.tsx" ]]; then
        echo "[setup-catalyst] ${SRC} not found; keeping the copy in src/components/catalyst"
        exit 0
    fi
    echo "[setup-catalyst] Catalyst not found in ${SRC}." >&2
    echo "[setup-catalyst] Set CATALYST_DIR to the kit's typescript/ directory," >&2
    echo "[setup-catalyst] or run ../scripts/fetch-webroot.sh to download a built panel." >&2
    exit 1
fi

rm -rf "${DEST}"
mkdir -p "${DEST}"
cp "${SRC}"/*.tsx "${DEST}/"
cp "${ROOT}"/catalyst-overrides/*.tsx "${DEST}/"
echo "[setup-catalyst] $(ls "${DEST}" | wc -l) components from ${SRC}"
