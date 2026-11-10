#!/usr/bin/env bash
# Stop the server, the controller and the device processes of this tree.
#
# Usage: scripts/stop-all.sh [server|controller|all]
#
#   server      the API only
#   controller  the manager and its device processes
#   all         both (default)
#
# SIGTERM first, the same signal Ctrl-C delivers to scripts/run-all.sh. The
# controller then stops its devices and resets their sessions; devices are
# signalled directly only if they are still running afterwards. A process
# still alive after the wait is killed. PostgreSQL is left running.
#
# scripts/run-all.sh exits when either process it started exits, and its trap
# stops the other one. Stopping one side while that script supervises both is
# refused; run scripts/stop-all.sh with no arguments instead.
#
# Variables (the same ones scripts/run-all.sh reads):
#   PGDEMO_BUILD_DIR (build)          binaries to match
#   PGDEMO_DEVICES_DIR (run/devices)  device directories to match
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${PGDEMO_BUILD_DIR:-${ROOT}/build}"
DEVICES="${PGDEMO_DEVICES_DIR:-${ROOT}/run/devices}"

canon() {
    if [[ -e "$1" ]]; then
        readlink -f "$1"
    else
        printf '%s' "$1"
    fi
}

server_bin="$(canon "${BUILD}/server/pgdemo-server")"
controller_bin="$(canon "${BUILD}/controller/pgdemo-controller")"
device_bin="$(canon "${BUILD}/device/pgdemo-device")"
devices_dir="$(canon "${DEVICES}")"
run_all="${ROOT}/scripts/run-all.sh"

say() { printf '[stop-all] %s\n' "$*"; }
die() { printf '[stop-all] ERROR: %s\n' "$*" >&2; exit 1; }

# State character from /proc/pid/stat. The command name is inside parentheses
# and may contain spaces, so the state is the field that follows the last ")".
proc_state() {
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
    stat="${stat##*) }"
    printf '%s' "${stat%% *}"
}

running() {
    local state
    state="$(proc_state "$1" 2>/dev/null)" || return 1
    [[ -n $state && $state != Z ]]
}

proc_exe() {
    local exe
    exe="$(readlink "/proc/$1/exe" 2>/dev/null)" || return 1
    printf '%s' "${exe% (deleted)}"
}

proc_cwd() {
    readlink -f "/proc/$1/cwd" 2>/dev/null || true
}

# Print the raw arguments of pid, one per line.
raw_args() {
    local arg
    while IFS= read -r -d '' arg; do
        printf '%s\n' "$arg"
    done < "/proc/$1/cmdline"
}

# Resolve one argument against the process cwd. Flags stay as written.
resolve_arg() {
    local cwd=$1 arg=$2 path
    path=$arg
    if [[ $path != /* && -n $cwd ]]; then
        path="${cwd}/${path}"
    fi
    if [[ -e $path ]]; then
        readlink -f "$path"
    else
        printf '%s' "$arg"
    fi
}

is_shell() {
    local base
    base="$(basename "$1")"
    [[ $base == bash || $base == dash || $base == sh || $base == run-all.sh ]]
}

# Scope of a run-all.sh process: server, controller or all.
run_all_scope() {
    local pid=$1 cwd seen=0 arg scope=all
    cwd="$(proc_cwd "$pid")"
    while IFS= read -r arg; do
        if (( seen )); then
            case $arg in
                server|controller|all) scope=$arg ;;
            esac
            continue
        fi
        [[ $(resolve_arg "$cwd" "$arg") == "$run_all" ]] && seen=1
    done < <(raw_args "$pid")
    printf '%s' "$scope"
}

is_run_all() {
    local pid=$1 exe cwd arg
    exe="$(proc_exe "$pid" 2>/dev/null)" || return 1
    is_shell "$exe" || return 1
    cwd="$(proc_cwd "$pid")"
    while IFS= read -r arg; do
        [[ $(resolve_arg "$cwd" "$arg") == "$run_all" ]] && return 0
    done < <(raw_args "$pid")
    return 1
}

device_in_tree() {
    local pid=$1 cwd dir seen=0 arg
    cwd="$(proc_cwd "$pid")"
    while IFS= read -r -d '' arg; do
        if (( seen )); then
            dir=$arg
            if [[ $dir != /* && -n $cwd ]]; then
                dir="${cwd}/${dir}"
            fi
            if [[ -e $dir ]]; then
                dir="$(readlink -f "$dir")"
            fi
            [[ $dir == "$devices_dir" || $dir == "$devices_dir"/* ]] && return 0
            return 1
        fi
        [[ $arg == --dir ]] && seen=1
    done < "/proc/${pid}/cmdline"
    return 1
}

# Print the pids of one kind: server, controller, device or run-all.
collect() {
    local kind=$1 pid path
    for pid in /proc/[0-9]*; do
        pid="${pid##*/}"
        [[ $pid == "$$" ]] && continue
        running "$pid" || continue
        case $kind in
            run-all)
                is_run_all "$pid" && printf '%s\n' "$pid"
                ;;
            server|controller|device)
                path="$(proc_exe "$pid" 2>/dev/null)" || continue
                case $kind in
                    server) [[ $path == "$server_bin" ]] && printf '%s\n' "$pid" ;;
                    controller) [[ $path == "$controller_bin" ]] && printf '%s\n' "$pid" ;;
                    device)
                        if [[ $path == "$device_bin" ]] || { [[ $(basename "$path") == pgdemo-device ]] && device_in_tree "$pid"; }; then
                            printf '%s\n' "$pid"
                        fi
                        ;;
                esac
                ;;
        esac
    done
}

signal_group() {
    local sig=$1 role=$2 pid
    shift 2
    for pid in "$@"; do
        if kill "-${sig}" "$pid" 2>/dev/null; then
            say "${sig} ${role} pid ${pid}"
        fi
    done
}

still_alive() {
    local pid
    for pid in "$@"; do
        running "$pid" && printf '%s\n' "$pid"
    done
}

wait_gone() {
    local seconds=$1
    shift
    local deadline=$((SECONDS + seconds)) left
    while (( SECONDS < deadline )); do
        left="$(still_alive "$@")"
        [[ -z $left ]] && return 0
        sleep 0.2
    done
    return 1
}

main() {
    local what=all
    while [[ $# -gt 0 ]]; do
        case $1 in
            server|controller|all) what=$1 ;;
            -h|--help)
                sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
                exit 0
                ;;
            *) die "unknown argument $1" ;;
        esac
        shift
    done
    [[ -d /proc/$$ ]] || die "/proc is not available"

    local -a server_pids=() controller_pids=() device_pids=() run_pids=()
    mapfile -t server_pids < <(collect server)
    mapfile -t controller_pids < <(collect controller)
    mapfile -t device_pids < <(collect device)
    mapfile -t run_pids < <(collect run-all)

    local pid scope
    if [[ $what != all ]]; then
        for pid in "${run_pids[@]}"; do
            scope="$(run_all_scope "$pid")"
            if [[ $scope == all ]]; then
                die "run-all.sh pid ${pid} is supervising both; run scripts/stop-all.sh"
            fi
        done
    fi

    local -a watch=() run_term=()
    case $what in
        server)
            watch=("${server_pids[@]}")
            ;;
        controller)
            watch=("${controller_pids[@]}")
            ;;
        all)
            watch=("${server_pids[@]}" "${controller_pids[@]}")
            ;;
    esac
    for pid in "${run_pids[@]}"; do
        scope="$(run_all_scope "$pid")"
        if [[ $what == all || $scope == "$what" ]]; then
            run_term+=("$pid")
            watch+=("$pid")
        fi
    done

    if (( ${#watch[@]} == 0 )) && { [[ $what == server ]] || (( ${#device_pids[@]} == 0 )); }; then
        say "nothing to stop"
        exit 0
    fi

    # The controller restarts a device that exits before it has been asked to
    # shut down, so the devices are signalled only once the controller is gone.
    [[ $what == controller || $what == all ]] && signal_group TERM controller "${controller_pids[@]}"
    [[ $what == server || $what == all ]] && signal_group TERM server "${server_pids[@]}"
    signal_group TERM run-all "${run_term[@]}"

    if (( ${#watch[@]} > 0 )); then
        wait_gone 12 "${watch[@]}" || true
    fi

    local -a left=()
    if (( ${#watch[@]} > 0 )); then
        mapfile -t left < <(still_alive "${watch[@]}")
        if (( ${#left[@]} > 0 )); then
            signal_group KILL process "${left[@]}"
            wait_gone 2 "${left[@]}" || true
        fi
    fi

    # Orphans only. A live controller would restart a device signalled directly.
    local -a controllers_left=()
    mapfile -t controllers_left < <(collect controller)
    if [[ $what == controller || $what == all ]] && (( ${#controllers_left[@]} == 0 )); then
        device_pids=()
        mapfile -t device_pids < <(collect device)
        if (( ${#device_pids[@]} > 0 )); then
            signal_group TERM device "${device_pids[@]}"
            wait_gone 3 "${device_pids[@]}" || true
            left=()
            mapfile -t left < <(still_alive "${device_pids[@]}")
            if (( ${#left[@]} > 0 )); then
                signal_group KILL device "${left[@]}"
                wait_gone 2 "${left[@]}" || true
            fi
            watch+=("${device_pids[@]}")
        fi
    fi

    left=()
    if (( ${#watch[@]} > 0 )); then
        mapfile -t left < <(still_alive "${watch[@]}")
    fi
    if (( ${#left[@]} > 0 )); then
        die "still running: ${left[*]}"
    fi
    say "stopped"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
