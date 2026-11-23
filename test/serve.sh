#!/usr/bin/env bash
# serve.sh DIR [ARG...]: start `./rayforce -p PORT ARG...` in the background on a
# TCP port no other process holds, and write PORT to DIR/port and its pid to
# DIR/pid once that pid is the only one listening there.  Exit 0 then, 1 when no
# port could be had, 2 when listeners cannot be inspected here.
#
# serve.sh --tie PID DIR: give an already started process (a test's helper) the
# same lifetime as a server: stopped when the test that asked for it is gone.
#
# A leading `--owner PID` names the process whose end stops the server (or the
# tied process) instead of the test process found from DIR (see owner_of).
#
# For .rfl tests that spawn servers (`(.sys.exec "test/serve.sh /tmp/x ...")`):
# a fixed port is shared by every suite on the host, so two suites running at
# once connected to, and killed, each other's servers (#1140).  A candidate is
# drawn at random below the Linux ephemeral range; one another process takes
# first makes our server's bind fail and the server exit, and the next one is
# tried.  The listener is checked to be ours alone, not merely open, so a client
# never reaches another suite's server.
#
# Cleanup does not depend on the test reaching its last line: a test stops at
# its first failing assertion, and a run can be interrupted.  Until the server
# is handed over, a trap stops it on any exit of this script; after, a watchdog
# stops it (TERM, then KILL) and removes DIR/pid and DIR/port as soon as the
# test process that asked for it is gone, or after RAY_SERVE_MAX_S seconds.  A
# test that ends normally still stops its server itself: `kill $(cat DIR/pid)`,
# never by pattern.  The server's stderr goes to DIR/server.log.
#
# bash 3.2 (macOS) compatible: no associative arrays, no ${var,,}, BSD ps/lsof.

RAY_SERVE_MAX_S=900

# The test process this script works for: the rfl runner's worker, whose
# per-process scratch root DIR lies under (TMPDIR/rayforce_t.<pid>/...);
# otherwise the nearest ancestor that is not a shell.
owner_of() {
    local d=$1 o
    case "$d" in
        */rayforce_t.[0-9]*)
            o=${d#*rayforce_t.}; o=${o%%/*}
            case "$o" in *[!0-9]*|'') ;; *) echo "$o"; return ;; esac ;;
    esac
    o=$PPID
    while [ -n "$o" ] && [ "$o" -gt 1 ]; do
        case "$(ps -o comm= -p "$o" 2>/dev/null)" in
            *sh|*bash|*dash|*zsh|sh|bash) o=$(ps -o ppid= -p "$o" 2>/dev/null | tr -d ' ') ;;
            *) echo "$o"; return ;;
        esac
    done
    echo "$PPID"
}

# Stop pid $1 (TERM, then KILL) once owner $2 is gone, $1 is gone, or the
# lifetime runs out; then remove the files named after it.  Detached, with
# INT/TERM/HUP of its group ignored: it outlives the test it guards.
watch() {
    local pid=$1 owner=$2; shift 2
    (
        trap '' INT TERM HUP
        t=0
        while kill -0 "$owner" 2>/dev/null && kill -0 "$pid" 2>/dev/null \
              && [ "$t" -lt $((RAY_SERVE_MAX_S * 5)) ]; do
            sleep 0.2
            t=$((t + 1))
        done
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null
            for _ in 1 2 3 4 5 6 7 8 9 10; do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.2
            done
            kill -KILL "$pid" 2>/dev/null
        fi
        rm -f "$@"
    ) </dev/null >/dev/null 2>&1 &
}

owner=
if [ "$1" = "--owner" ]; then
    owner=$2
    shift 2
fi

if [ "$1" = "--tie" ]; then
    [ -n "$2" ] && [ -n "$3" ] || exit 1
    [ -n "$owner" ] || owner=$(owner_of "$3")
    watch "$2" "$owner"
    exit 0
fi

dir=$1
shift
mkdir -p "$dir" || exit 1
[ -n "$owner" ] || owner=$(owner_of "$dir")

# Whether pid $1 is the only process listening on TCP port $2.
if command -v ss >/dev/null 2>&1 && ss -Hltn >/dev/null 2>&1; then
    owns() {
        local l
        l=$(ss -Hltnp "sport = :$2" 2>/dev/null)
        [ -n "$l" ] && ! printf '%s\n' "$l" | grep -qv "pid=$1,"
    }
elif command -v lsof >/dev/null 2>&1; then
    owns() {
        local l
        l=$(lsof -nP -t -iTCP:"$2" -sTCP:LISTEN 2>/dev/null | sort -u)
        [ "$l" = "$1" ]
    }
else
    echo "serve.sh: neither ss nor lsof can list listeners here" >&2
    exit 2
fi

pid=
cleanup() {
    if [ -n "$pid" ]; then
        kill -KILL "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for _ in $(seq 40); do
    port=$((20000 + RANDOM % 12000))
    (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && continue   # taken
    ./rayforce -p "$port" "$@" </dev/null >/dev/null 2>>"$dir/server.log" &
    pid=$!
    for _ in $(seq 200); do
        kill -0 "$pid" 2>/dev/null || break                          # its bind failed
        if owns "$pid" "$port"; then
            echo "$port" > "$dir/port"
            echo "$pid" > "$dir/pid"
            watch "$pid" "$owner" "$dir/pid" "$dir/port"
            pid=                                                     # handed over
            exit 0
        fi
        sleep 0.05
    done
    cleanup
    pid=
done
exit 1
