#!/usr/bin/env bash
# Self-signed ECDSA certificate for the HTTPS listener. This is not the JWT
# key: tokens and TLS must not share a private key.
#
# Usage: scripts/gen-tls-cert.sh [--force] [DIR]     (default DIR: run/keys)
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

KEY="${DIR}/tls.key"
CRT="${DIR}/tls.crt"

if [[ -f "${KEY}" && -f "${CRT}" && "${FORCE}" -eq 0 ]]; then
    echo "[gen-tls-cert] ${KEY#"${ROOT}"/} already exists (use --force to rotate)"
    exit 0
fi

mkdir -p "${DIR}"
chmod 700 "${DIR}"
umask 077
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "${KEY}"
openssl req -new -x509 -key "${KEY}" -sha256 -days 365 \
    -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
    -out "${CRT}"
chmod 600 "${KEY}"
chmod 644 "${CRT}"
echo "[gen-tls-cert] wrote ${KEY#"${ROOT}"/} and ${CRT#"${ROOT}"/}"
