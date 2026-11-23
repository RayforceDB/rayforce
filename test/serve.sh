#!/usr/bin/env bash
# serve.sh [--owner PID] [--listen] DIR [ARG...]
#   Start `./rayforce -p PORT ARG...` in the background on a TCP port no other
#   process holds.  Once that pid is the only one listening there, and (without
#   --listen) answers the IPC handshake, write PORT to DIR/port, the pid to
#   DIR/pid and its start time to DIR/start.  Exit 0 then, 1 when no port could
#   be had, 2 when listeners cannot be inspected here.  The server's stdout and
#   stderr go to DIR/server.log.  --listen is for a server that refuses an
#   anonymous handshake (-u/-U): it is ready once it listens.
# serve.sh --stop DIR [SIG]
#   Send SIG (default TERM) to the server DIR names, only if it is still the
#   process that was started (its start time is unchanged), and wait until it
#   is gone.  Exit 0 when it is gone (or was already), 1 when it is not.
# serve.sh [--owner PID] --tie PID DIR
#   Give an already started process (a test's helper) the same lifetime as a
#   server: stopped when the test that asked for it is gone.
# serve.sh --port
#   Print a port nothing listens on, for a test that needs a refused connect or
#   starts a one-shot process on a port of its own.  Drawn from 10000-18999,
#   apart from the servers' range, so no server of another suite takes it.
#
# For .rfl tests that spawn servers (`(.sys.exec "test/serve.sh /tmp/x ...")`):
# a fixed port is shared by every suite on the host, so two suites running at
# once connected to, and killed, each other's servers (#1140).  A candidate is
# drawn at random in 20000-31999, below the Linux ephemeral range; one another
# process takes first makes our server's bind fail and the server exit, and the
# next one is tried.  The listener is checked to be ours alone, not merely
# open, so a client never reaches another suite's server.
#
# Cleanup does not depend on the test reaching its last line: a test stops at
# its first failing assertion, and a run can be interrupted.  Until the server
# is handed over, a trap stops it on any exit of this script; after, a watchdog
# stops it (TERM, then KILL) and removes DIR/pid, DIR/port and DIR/start as soon
# as the test process that asked for it is gone, or after RAY_SERVE_MAX_S
# seconds.  A pid is taken for the same process only while its start time is
# the one recorded, so neither the watchdog nor --stop signals a process that
# reused the pid.  A test that ends normally stops its server itself
# (--stop), never by pattern.
#
# bash 3.2 (macOS) compatible: no associative arrays, no ${var,,}, BSD ps/lsof.

RAY_SERVE_MAX_S=900

# The start time of pid $1 (empty when it is gone): with the pid, what tells
# one process from a later one that reused its pid.
started() { ps -o lstart= -p "$1" 2>/dev/null; }

# Whether pid $1 is alive and still the process that started at $2.
same() { [ -n "$2" ] && [ "$(started "$1")" = "$2" ]; }

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

# Stop pid $1, which started at $2, once owner $3 is gone, $1 is gone, or the
# lifetime runs out; then remove the files named after them.  kill -0 every
# 0.2 s, and every second the start times, which catch a reused pid.
# Detached, with INT/TERM/HUP of its group ignored: it outlives the test it
# guards.
watch() {
    local pid=$1 pstart=$2 owner=$3 ostart
    shift 3
    ostart=$(started "$owner")
    (
        trap '' INT TERM HUP
        t=0
        while kill -0 "$owner" 2>/dev/null && kill -0 "$pid" 2>/dev/null \
              && [ "$t" -lt $((RAY_SERVE_MAX_S * 5)) ]; do
            if [ $((t % 5)) -eq 0 ]; then
                same "$owner" "$ostart" || break
                same "$pid" "$pstart" || break
            fi
            sleep 0.2
            t=$((t + 1))
        done
        if same "$pid" "$pstart"; then
            kill -TERM "$pid" 2>/dev/null
            for _ in 1 2 3 4 5 6 7 8 9 10; do
                same "$pid" "$pstart" || break
                sleep 0.2
            done
            same "$pid" "$pstart" && kill -KILL "$pid" 2>/dev/null
        fi
        rm -f "$@"
    ) </dev/null >/dev/null 2>&1 &
}

# Whether nothing listens on TCP port $1 of the loopback.
free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

case "$1" in
    --port)
        for _ in $(seq 100); do
            p=$((10000 + RANDOM % 9000))
            free "$p" && { echo "$p"; exit 0; }
        done
        exit 1 ;;
    --stop)
        d=$2; sig=${3:-TERM}
        [ -f "$d/pid" ] || exit 0
        p=$(cat "$d/pid"); s=$(cat "$d/start" 2>/dev/null)
        same "$p" "$s" || exit 0
        kill -"$sig" "$p" 2>/dev/null
        for _ in $(seq 200); do
            same "$p" "$s" || exit 0
            sleep 0.05
        done
        exit 1 ;;
esac

owner=
listen_only=
while :; do
    case "$1" in
        --owner) owner=$2; shift 2 ;;
        --listen) listen_only=1; shift ;;
        *) break ;;
    esac
done

if [ "$1" = "--tie" ]; then
    [ -n "$2" ] && [ -n "$3" ] || exit 1
    [ -n "$owner" ] || owner=$(owner_of "$3")
    watch "$2" "$(started "$2")" "$owner"
    exit 0
fi

dir=$1
shift
mkdir -p "$dir" || exit 1
[ -n "$owner" ] || owner=$(owner_of "$dir")

# Whether every socket listening on TCP port $2 is one pid $1 holds: a socket
# is the server's even when a child it spawned (a .sys.exec command) inherited
# it.  ss prints each socket once with all its holders; lsof prints a row per
# holder, so its socket addresses (the d field) are compared instead of pids.
if command -v ss >/dev/null 2>&1 && ss -Hltn >/dev/null 2>&1; then
    owns() {
        local l
        l=$(ss -Hltnp "sport = :$2" 2>/dev/null)
        [ -n "$l" ] && ! printf '%s\n' "$l" | grep -qv "pid=$1,"
    }
elif command -v lsof >/dev/null 2>&1; then
    owns() {
        lsof -nP -iTCP:"$2" -sTCP:LISTEN -F pd 2>/dev/null | awk -v want="$1" '
            /^p/ { p = substr($0, 2) }
            /^d/ { d = substr($0, 2); all[d] = 1; if (p == want) mine[d] = 1 }
            END  { n = 0
                   for (d in all) { if (!(d in mine)) exit 1; n++ }
                   exit n == 0 }'
    }
else
    echo "serve.sh: neither ss nor lsof can list listeners here" >&2
    exit 2
fi

# Whether the server on port $1 answers the IPC handshake (version byte and an
# empty credential): listen(2) comes before the startup script and the poll
# loop, and an .ipc.open sent in between can time out.
answers() {
    bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1" && printf "\004\000" >&3 && read -t 2 -n 1 -u 3 _' \
        _ "$1" 2>/dev/null
}

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
    free "$port" || continue
    ./rayforce -p "$port" "$@" </dev/null >>"$dir/server.log" 2>&1 &
    pid=$!
    for _ in $(seq 200); do
        kill -0 "$pid" 2>/dev/null || break                          # its bind failed
        if owns "$pid" "$port"; then
            if [ -z "$listen_only" ]; then
                ok=
                for _ in $(seq 100); do
                    answers "$port" && { ok=1; break; }
                    kill -0 "$pid" 2>/dev/null || break
                    sleep 0.1
                done
                [ -n "$ok" ] || exit 1                               # the trap stops it
            fi
            start=$(started "$pid")
            echo "$port" > "$dir/port"
            echo "$pid" > "$dir/pid"
            echo "$start" > "$dir/start"
            watch "$pid" "$start" "$owner" "$dir/pid" "$dir/port" "$dir/start"
            pid=                                                     # handed over
            exit 0
        fi
        sleep 0.05
    done
    cleanup
    pid=
done
exit 1
