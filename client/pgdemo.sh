#!/usr/bin/env bash
# pgdemo command-line client (curl + jq).
#
# Usage: client/pgdemo.sh [--http] <command> [args]
#
#   (default)  https://127.0.0.1:8443, TLS 1.3   server started with scripts/run-all.sh
#   --http     http://127.0.0.1:8080, plain     server started with scripts/run-all.sh --http
#
#   login [USER]                      password from $PGDEMO_PASSWORD or a prompt
#   logout
#   me
#   health
#   info                              PostgreSQL, libpqxx and listener details
#
#   devices                           every device and its last sample
#   devices types                     device types: payload and default configuration
#   devices add NAME TYPE             admin; TYPE: sine | sawtooth | gps_orbit; waits for
#                                     the certificate (201), or 202 if still enrolling
#   devices del NAME                  admin; the telemetry is kept
#   devices certs NAME                every certificate the device has had, newest first
#   devices revoke NAME REASON        admin; REASON: unspecified | keyCompromise |
#                                     affiliationChanged | cessationOfOperation |
#                                     certificateHold (the only one that can be lifted)
#   devices unrevoke NAME             admin; lift a certificateHold
#   devices rotate NAME               admin; new key and certificate, the old one superseded
#   devices delcert NAME SERIAL       admin; deleting the current one deletes the device
#   devices enrollments [NAME]        enrollment and rotation requests, newest first
#   data DEVICE                       one device: last sample, config sync state, identity
#   history DEVICE [limit=N] [from=TS] [to=TS]
#   monitor [CHANNELS] [DEVICES]      live SSE; CHANNELS from telemetry_scalar,
#                                     telemetry_gps,telemetry_xy,telemetry_xyz,device,config,enrollment,error (default all),
#                                     DEVICES a comma-separated filter (default all)
#
#   config status                     applied vs desired, per device
#   config set DEVICE [rate_ms=N] [amplitude=X] [active=true|false]
#   config request ID
#   config history [DEVICE] [limit=N]
#   burst N [DEVICE]                  N concurrent `config set` (Dispatcher demo)
#
#   users list                        admin only
#   users add NAME [admin|normal]     password from $PGDEMO_NEW_PASSWORD or a prompt
#   users del NAME
#
#   pki ca                            root CAs, the signing one, and the revoked serials
#   pki crl                           the CRL in PEM: pgdemo.sh pki crl | openssl crl -text
#
# Environment:
#   PGDEMO_URL    base URL override, for Docker and the e2e suite (host port 8080, HTTPS)
#   PGDEMO_CA     TLS CA file (default <repo>/run/keys/tls.crt)
#   PGDEMO_TOKEN  token file (default ${XDG_RUNTIME_DIR:-~/.cache}/pgdemo/token)
#
# The token is stored as a ready-made "Authorization:" header in a 0600 file
# and passed with `curl -H @file`, so it never appears in a process listing.
# Passwords are piped to jq on stdin for the same reason.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
URL=https://127.0.0.1:8443
if [[ "${1:-}" == --http ]]; then
    URL=http://127.0.0.1:8080
    shift
fi
URL="${PGDEMO_URL:-${URL}}"
CA="${PGDEMO_CA:-${ROOT}/run/keys/tls.crt}"
TOKEN_FILE="${PGDEMO_TOKEN:-${XDG_RUNTIME_DIR:-${HOME}/.cache}/pgdemo/token}"

die() { printf 'pgdemo: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null || die "$1 not found (apt install $1)"; }
need curl
need jq

usage() { sed -n '2,54p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

auth_args() {
    [[ -f "${TOKEN_FILE}" ]] || die "not logged in (client/pgdemo.sh login)"
    printf '%s\n' "-H" "@${TOKEN_FILE}"
}

# api METHOD PATH [curl args...]; body, if any, on stdin via --data-binary @-
# Prints the JSON body; returns 1 for HTTP >= 400.
api() {
    local method="$1" path="$2"
    shift 2
    local out code
    out="$(mktemp)"
    code="$(curl -sS --cacert "${CA}" -o "${out}" -w '%{http_code}' -X "${method}" "$@" "${URL}${path}")" || {
        rm -f "${out}"
        die "cannot reach ${URL}"
    }
    if jq -e . "${out}" >/dev/null 2>&1; then jq . "${out}"; else cat "${out}"; fi
    rm -f "${out}"
    printf 'HTTP %s\n' "${code}" >&2
    [[ "${code}" -lt 400 ]]
}

authed() {
    local method="$1" path="$2"
    shift 2
    local -a a
    mapfile -t a < <(auth_args)
    api "${method}" "${path}" "${a[@]}" "$@"
}

json_body() { printf '%s' "$1" | jq -c .; }

read_secret() {
    local prompt="$1" var="$2" value
    if [[ -n "${!var:-}" ]]; then printf '%s' "${!var}"; return; fi
    [[ -t 0 ]] || die "set ${var} or run interactively"
    read -rsp "${prompt}" value
    echo >&2
    printf '%s' "${value}"
}

# key=value pairs to a query string (values are URL-encoded by jq).
query() {
    local q="" kv k v
    for kv in "$@"; do
        [[ "${kv}" == *=* ]] || die "expected key=value, got '${kv}'"
        k="${kv%%=*}"
        v="$(printf '%s' "${kv#*=}" | jq -sRr @uri)"
        q+="${q:+&}${k}=${v}"
    done
    [[ -n "${q}" ]] && printf '?%s' "${q}"
    return 0
}

cmd_login() {
    local user="${1:-admin}" pw token
    pw="$(read_secret "password for ${user}: " PGDEMO_PASSWORD)"
    local resp
    resp="$(printf '%s' "${pw}" | jq -Rsc --arg u "${user}" '{username: $u, password: .}' |
        api POST /api/auth/login -H 'Content-Type: application/json' --data-binary @-)" || {
        printf '%s\n' "${resp}"
        exit 1
    }
    token="$(jq -r .token <<<"${resp}")"
    mkdir -p "$(dirname "${TOKEN_FILE}")"
    (umask 077 && printf 'Authorization: Bearer %s\n' "${token}" > "${TOKEN_FILE}")
    jq '{role, expires_in}' <<<"${resp}"
}

config_set_body() {
    local device="$1"
    shift
    local body
    body="$(jq -nc --arg d "${device}" '{device: $d}')"
    local kv k v
    for kv in "$@"; do
        k="${kv%%=*}"
        v="${kv#*=}"
        case "${k}" in
            rate_ms|rate) body="$(jq -c --argjson v "${v}" '.rate_ms = $v' <<<"${body}")" ;;
            amplitude|amp) body="$(jq -c --argjson v "${v}" '.amplitude = $v' <<<"${body}")" ;;
            active|is_active) body="$(jq -c --argjson v "${v}" '.is_active = $v' <<<"${body}")" ;;
            *) die "unknown config field '${k}'" ;;
        esac
    done
    printf '%s' "${body}"
}

cmd_config() {
    local sub="${1:-status}"
    shift || true
    case "${sub}" in
        status) authed GET /api/config ;;
        set)
            [[ $# -ge 1 ]] || die "config set DEVICE [rate_ms=N] [amplitude=X] [active=true|false]"
            config_set_body "$@" | authed POST /api/config -H 'Content-Type: application/json' --data-binary @-
            ;;
        request) [[ $# -eq 1 ]] || die "config request ID"; authed GET "/api/config/requests/$1" ;;
        history)
            local args=()
            if [[ $# -ge 1 && "$1" != *=* ]]; then args+=("device=$1"); shift; fi
            authed GET "/api/config/history$(query "${args[@]}" "$@")"
            ;;
        *) die "unknown config command '${sub}'" ;;
    esac
}

cmd_burst() {
    local n="${1:-10}" device="${2:-dummy1}" i start
    [[ "${n}" =~ ^[0-9]+$ && "${n}" -ge 1 && "${n}" -le 500 ]] || die "burst N with N in 1..500"
    local -a a
    mapfile -t a < <(auth_args)
    local dir
    dir="$(mktemp -d)"
    start="$(date +%s%N)"
    for ((i = 0; i < n; i++)); do
        # Every 5th request is out of range, so the controller rejects it.
        local rate=$((100 + i * 10))
        ((i % 5 == 4)) && rate=5
        (curl -sS --cacert "${CA}" -o "${dir}/${i}.json" -w '%{http_code}\n' -X POST "${a[@]}" \
            -H 'Content-Type: application/json' \
            --data-binary "{\"device\":\"${device}\",\"rate_ms\":${rate}}" \
            "${URL}/api/config" > "${dir}/${i}.code") &
    done
    wait
    local ms=$((($(date +%s%N) - start) / 1000000))
    cat "${dir}"/*.code | sort | uniq -c | awk '{printf "  HTTP %s x %s\n", $2, $1}'
    jq -s -c 'map({request_id, status}) | sort_by(.request_id)' "${dir}"/*.json
    echo "${n} request(s) in ${ms} ms"
    rm -rf "${dir}"
}

cmd_monitor() {
    local channels="${1:-telemetry_scalar,telemetry_gps,telemetry_xy,telemetry_xyz,device,config,enrollment,error}" devices="${2:-}"
    local -a a
    mapfile -t a < <(auth_args)
    echo "streaming ${channels}${devices:+ for ${devices}} (Ctrl-C to stop)" >&2
    local event="" line
    # Process substitution, not a pipe: curl's HTTPS stdout is block-buffered
    # when piped, so data frames never reached this loop.
    while IFS= read -r line || [[ -n "${line}" ]]; do
        line="${line%$'\r'}"
        case "${line}" in
            event:*) event="${line#event: }" ;;
            data:*) printf '%-16s %s\n' "${event}" "$(jq -c . <<<"${line#data: }")" || true ;;
        esac
    done < <(curl --http1.1 -sSN --cacert "${CA}" "${a[@]}" \
        "${URL}/api/stream?channels=${channels}${devices:+&devices=${devices}}")
}

cmd_devices() {
    local sub="${1:-list}"
    shift || true
    case "${sub}" in
        list) authed GET /api/devices ;;
        types) authed GET /api/device-types ;;
        add)
            [[ $# -eq 2 ]] || die "devices add NAME TYPE"
            jq -nc --arg n "$1" --arg t "$2" '{name: $n, type: $t}' |
                authed POST /api/devices -H 'Content-Type: application/json' --data-binary @-
            ;;
        del|delete) [[ $# -eq 1 ]] || die "devices del NAME"; authed DELETE "/api/devices/$1" ;;
        certs) [[ $# -eq 1 ]] || die "devices certs NAME"; authed GET "/api/devices/$1/certificates" ;;
        revoke)
            [[ $# -eq 2 ]] || die "devices revoke NAME REASON"
            jq -nc --arg r "$2" '{reason: $r}' |
                authed POST "/api/devices/$1/revoke" -H 'Content-Type: application/json' --data-binary @-
            ;;
        unrevoke) [[ $# -eq 1 ]] || die "devices unrevoke NAME"; authed DELETE "/api/devices/$1/revoke" ;;
        rotate) [[ $# -eq 1 ]] || die "devices rotate NAME"; authed POST "/api/devices/$1/rotate" ;;
        delcert)
            [[ $# -eq 2 ]] || die "devices delcert NAME SERIAL"
            authed DELETE "/api/devices/$1/certificates/$2"
            ;;
        enrollments)
            local args=()
            [[ $# -ge 1 ]] && args+=("device=$1")
            authed GET "/api/enrollments$(query "${args[@]}")"
            ;;
        *) die "unknown devices command '${sub}'" ;;
    esac
}

cmd_pki() {
    local sub="${1:-ca}"
    shift || true
    case "${sub}" in
        ca) authed GET /api/pki/ca ;;
        crl) authed GET /api/pki/crl ;;
        *) die "unknown pki command '${sub}'" ;;
    esac
}

cmd_users() {
    local sub="${1:-list}"
    shift || true
    case "${sub}" in
        list) authed GET /api/users ;;
        add)
            [[ $# -ge 1 ]] || die "users add NAME [admin|normal]"
            local name="$1" role="${2:-normal}" pw
            pw="$(read_secret "password for new user ${name}: " PGDEMO_NEW_PASSWORD)"
            printf '%s' "${pw}" | jq -Rsc --arg u "${name}" --arg r "${role}" \
                '{username: $u, password: ., role: $r}' |
                authed POST /api/users -H 'Content-Type: application/json' --data-binary @-
            ;;
        del|delete) [[ $# -eq 1 ]] || die "users del NAME"; authed DELETE "/api/users/$1" ;;
        *) die "unknown users command '${sub}'" ;;
    esac
}

main() {
    local cmd="${1:-help}"
    shift || true
    case "${cmd}" in
        login) cmd_login "$@" ;;
        logout) rm -f "${TOKEN_FILE}"; echo "logged out" ;;
        me) authed GET /api/auth/me ;;
        health) api GET /api/health ;;
        info) authed GET /api/db/info ;;
        devices) cmd_devices "$@" ;;
        data) [[ $# -eq 1 ]] || die "data DEVICE"; authed GET "/api/devices/$1" ;;
        history)
            [[ $# -ge 1 ]] || die "history DEVICE [limit=N] [from=TS] [to=TS]"
            local d="$1"
            shift
            authed GET "/api/devices/${d}/history$(query "$@")"
            ;;
        monitor) cmd_monitor "$@" ;;
        config) cmd_config "$@" ;;
        burst) cmd_burst "$@" ;;
        users) cmd_users "$@" ;;
        pki) cmd_pki "$@" ;;
        help|-h|--help) usage ;;
        *) usage; exit 2 ;;
    esac
}

main "$@"
