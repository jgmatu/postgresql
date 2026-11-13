#!/usr/bin/env bash
# Generate the factory root: a P-384 key (PKCS#8) and a self-signed certificate
# allowed to sign end-entity certificates only (pathlen:0). pgdemo-factory signs
# with the key; the database stores only the certificate. This script is not
# called by db-init.sh, and the factory process does not create the key itself.
#
# Usage: scripts/gen-factory.sh [--force] [DIR]     (default DIR: run/factory)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORCE=0
DIR="${ROOT}/run/factory"
for arg in "$@"; do
    case "${arg}" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) DIR="${arg}" ;;
    esac
done

KEY="${DIR}/root.key"
CRT="${DIR}/root.crt"

if [[ -f "${KEY}" && -f "${CRT}" && "${FORCE}" -eq 0 ]]; then
    echo "[gen-factory] ${CRT#"${ROOT}"/} already exists (use --force to replace)"
    exit 0
fi

mkdir -p "${DIR}"
chmod 700 "${DIR}"
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "${KEY}"
# SHA-256, not SHA-384: DDS-Security's PKI-DH plugin accepts ecdsa-with-SHA256 only.
# The key stays P-384.
openssl req -new -x509 -key "${KEY}" -sha256 -days 3650 \
    -subj "/O=pgdemo/CN=pgdemo factory root CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash" \
    -out "${CRT}" 2>/dev/null
chmod 600 "${KEY}"
chmod 644 "${CRT}"
echo "[gen-factory] wrote ${KEY#"${ROOT}"/} and ${CRT#"${ROOT}"/}"
