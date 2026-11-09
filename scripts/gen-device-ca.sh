#!/usr/bin/env bash
# Generate the device root CA: a P-384 key (PKCS#8) and a self-signed
# certificate allowed to sign end-entity certificates only (pathlen:0).
# pgdemo-server signs device CSRs with the key; the database stores only the
# certificate (root_ca), and the controller verifies device chains against it.
#
# Usage: scripts/gen-device-ca.sh [--force] [DIR]     (default DIR: run/ca)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORCE=0
DIR="${ROOT}/run/ca"
for arg in "$@"; do
    case "${arg}" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) DIR="${arg}" ;;
    esac
done

KEY="${DIR}/ca.key"
CRT="${DIR}/ca.crt"

if [[ -f "${KEY}" && -f "${CRT}" && "${FORCE}" -eq 0 ]]; then
    echo "[gen-device-ca] ${CRT#"${ROOT}"/} already exists (use --force to replace)"
    exit 0
fi

mkdir -p "${DIR}"
chmod 700 "${DIR}"
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "${KEY}"
openssl req -new -x509 -key "${KEY}" -sha384 -days 3650 \
    -subj "/O=pgdemo/CN=pgdemo device root CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash" \
    -out "${CRT}" 2>/dev/null
chmod 600 "${KEY}"
chmod 644 "${CRT}"
echo "[gen-device-ca] wrote ${KEY#"${ROOT}"/} and ${CRT#"${ROOT}"/}"
