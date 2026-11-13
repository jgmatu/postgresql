#!/usr/bin/env bash
# Generate the manager's P-384 key and its self-signed root before
# pgdemo-controller starts. The process does not create this key. The
# certificate is the manager's own root: it signs the CRL and is not issued
# by the factory. An existing key is kept. A certificate that is still the
# one the factory signed is rewritten from that same key.
#
# Usage: controller/scripts/gen-manager-key.sh [--force] [DIR]   (default DIR: run/controller)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FORCE=0
DIR="${ROOT}/run/controller"
for arg in "$@"; do
    case "${arg}" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) DIR="${arg}" ;;
    esac
done

KEY="${DIR}/manager.key"
CRT="${DIR}/manager.crt"

is_manager_root() {
    [[ -f "${CRT}" ]] || return 1
    local subject issuer text
    subject="$(openssl x509 -in "${CRT}" -noout -subject -nameopt RFC2253 | sed 's/^subject=//')"
    issuer="$(openssl x509 -in "${CRT}" -noout -issuer -nameopt RFC2253 | sed 's/^issuer=//')"
    [[ "${subject}" == "${issuer}" ]] || return 1
    [[ "${subject}" == *"CN=pgdemo-manager"* ]] || return 1
    text="$(openssl x509 -in "${CRT}" -noout -text)"
    [[ "${text}" == *"CA:TRUE"* && "${text}" == *"ecdsa-with-SHA256"* ]]
}

if [[ "${FORCE}" -eq 0 && -f "${KEY}" ]] && is_manager_root; then
    echo "[gen-manager-key] ${CRT#"${ROOT}"/} already exists (use --force to replace)"
    exit 0
fi

mkdir -p "${DIR}"
chmod 700 "${DIR}"
umask 077
if [[ ! -f "${KEY}" || "${FORCE}" -eq 1 ]]; then
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "${KEY}"
    chmod 600 "${KEY}"
    echo "[gen-manager-key] wrote ${KEY#"${ROOT}"/}"
fi
openssl req -new -x509 -key "${KEY}" -sha256 -days 3650 \
    -subj "/CN=pgdemo-manager" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash" \
    -out "${CRT}.tmp"
mv -f "${CRT}.tmp" "${CRT}"
chmod 644 "${CRT}"
echo "[gen-manager-key] wrote ${CRT#"${ROOT}"/}"
