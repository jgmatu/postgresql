#!/usr/bin/env bash
# Download the built control panel into webroot/, for a clone without the
# Catalyst sources that `pnpm build` in frontend/ needs.
#
# Usage: scripts/fetch-webroot.sh [--tag TAG] [--repo OWNER/NAME]
#
#   --tag TAG    Release to download from (default webroot-latest, which CI
#                updates on every push to main after the Robot suite passes)
#   --repo R     GitHub repository (default jgmatu/postgresql)
#
# Fetches webroot.tar.gz and webroot.tar.gz.sha256 without authentication,
# checks the sum and replaces everything in webroot/ except README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEBROOT="${ROOT}/webroot"
TAG=webroot-latest
REPO=jgmatu/postgresql

die() { printf '[fetch-webroot] ERROR: %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag) [[ $# -ge 2 ]] || die "--tag needs a value"; TAG="$2"; shift ;;
        --repo) [[ $# -ge 2 ]] || die "--repo needs a value"; REPO="$2"; shift ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument $1" ;;
    esac
    shift
done

command -v curl >/dev/null || die "curl not found"
command -v sha256sum >/dev/null || die "sha256sum not found"

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
base="https://github.com/${REPO}/releases/download/${TAG}"

echo "[fetch-webroot] ${base}/webroot.tar.gz"
curl -fsSL --retry 3 -o "${tmp}/webroot.tar.gz" "${base}/webroot.tar.gz" ||
    die "download failed; does the Release ${TAG} exist in ${REPO}?"
curl -fsSL --retry 3 -o "${tmp}/webroot.tar.gz.sha256" "${base}/webroot.tar.gz.sha256" ||
    die "checksum download failed"

expected="$(awk '{print $1; exit}' "${tmp}/webroot.tar.gz.sha256")"
actual="$(sha256sum "${tmp}/webroot.tar.gz" | awk '{print $1}')"
[[ "${expected}" =~ ^[0-9a-f]{64}$ ]] || die "malformed webroot.tar.gz.sha256"
[[ "${expected}" == "${actual}" ]] || die "sha256 mismatch: expected ${expected}, got ${actual}"

mkdir -p "${tmp}/x"
tar -xzf "${tmp}/webroot.tar.gz" -C "${tmp}/x" --no-same-owner
[[ -f "${tmp}/x/index.html" ]] || die "the archive has no index.html"
if find "${tmp}/x" \( -type l -o -name '*.map' \) | grep -q .; then
    die "the archive has symlinks or source maps"
fi

mkdir -p "${WEBROOT}"
find "${WEBROOT}" -mindepth 1 -maxdepth 1 ! -name README.md -exec rm -rf {} +
find "${tmp}/x" -mindepth 1 -maxdepth 1 ! -name README.md -exec cp -a {} "${WEBROOT}/" \;

echo "[fetch-webroot] $(find "${WEBROOT}" -type f | wc -l) files in ${WEBROOT} ($(tr '\n' ' ' < "${WEBROOT}/VERSION" 2>/dev/null || echo 'no VERSION'))"
