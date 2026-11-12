#!/usr/bin/env bash
# Generate the manager's P-384 key before pgdemo-controller starts. The process
# does not create this key. The factory signs the certificate the first time
# the controller starts with the key in place.
#
# Usage: controller/scripts/gen-manager-key.sh [--force] [DIR]   (default DIR: run/controller)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FORCE=0
DIR="${ROOT}/run/controller"
for arg in "$@"; do
    case "${arg}" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) DIR="${arg}" ;;
    esac
done

KEY="${DIR}/manager.key"
if [[ -f "${KEY}" && "${FORCE}" -eq 0 ]]; then
    echo "[gen-manager-key] ${KEY#"${ROOT}"/} already exists (use --force to replace)"
    exit 0
fi

mkdir -p "${DIR}"
chmod 700 "${DIR}"
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "${KEY}"
chmod 600 "${KEY}"
echo "[gen-manager-key] wrote ${KEY#"${ROOT}"/}"
