#!/usr/bin/env bash
# Generate the ES384 signing key (PKCS#8, P-384) and a self-signed certificate
# carrying its public key. The server signs tokens with the key and verifies
# them with the certificate.
#
# Usage: scripts/gen-jwt-keys.sh [--force] [DIR]     (default DIR: run/keys)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORCE=0
DIR="${ROOT}/run/keys"
for arg in "$@"; do
    case "${arg}" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) DIR="${arg}" ;;
    esac
done

KEY="${DIR}/jwt.key"
CRT="${DIR}/jwt.crt"

if [[ -f "${KEY}" && -f "${CRT}" && "${FORCE}" -eq 0 ]]; then
    echo "[gen-jwt-keys] ${KEY#"${ROOT}"/} already exists (use --force to rotate)"
    exit 0
fi

mkdir -p "${DIR}"
chmod 700 "${DIR}"
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "${KEY}"
openssl req -new -x509 -key "${KEY}" -sha384 -days 365 -subj "/CN=pgdemo-jwt" -out "${CRT}" 2>/dev/null
chmod 600 "${KEY}"
chmod 644 "${CRT}"
echo "[gen-jwt-keys] wrote ${KEY#"${ROOT}"/} and ${CRT#"${ROOT}"/}"
