#!/usr/bin/env bash
# After `next build`: check the static export and copy it to ../webroot/, the
# directory pgdemo-server serves (--webroot) and CI publishes.
#
# The export must not carry the Catalyst sources (see setup-catalyst.sh): no
# source maps, and none of the kit's module source text.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/out"
WEBROOT="$(cd "${ROOT}/.." && pwd)/webroot"

die() { echo "[export-webroot] $*" >&2; exit 1; }

[[ -f "${OUT}/index.html" ]] || die "${OUT}/index.html missing; did next build run?"

maps="$(find "${OUT}" -name '*.map' | head -5)"
[[ -z "${maps}" ]] || die "source maps in the export (they would publish the Catalyst sources): ${maps}"

if grep -rlE "import \* as Headless from|from '@headlessui/react'|catalyst-ui-kit" "${OUT}" >/dev/null 2>&1; then
    die "Catalyst source text found in the export: $(grep -rlE "import \* as Headless from|from '@headlessui/react'|catalyst-ui-kit" "${OUT}" | head -3)"
fi

mkdir -p "${WEBROOT}"
find "${WEBROOT}" -mindepth 1 -maxdepth 1 ! -name README.md -exec rm -rf {} +
cp -a "${OUT}/." "${WEBROOT}/"

commit="$(git -C "${ROOT}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [[ "${commit}" != unknown && -n "$(git -C "${ROOT}" status --porcelain -- . 2>/dev/null)" ]]; then
    commit="${commit}-dirty"
fi
printf 'commit=%s\nbuilt_at=%s\n' "${commit}" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "${WEBROOT}/VERSION"

echo "[export-webroot] $(find "${WEBROOT}" -type f | wc -l) files in ${WEBROOT} (commit ${commit})"
