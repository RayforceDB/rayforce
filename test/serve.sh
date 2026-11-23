#!/usr/bin/env bash
# serve.sh DIR [ARG...]: start `./rayforce -p PORT ARG...` in the background on a
# TCP port no other process holds, and write PORT to DIR/port and its pid to
# DIR/pid once that pid is the one listening there.  Exit 0 then, 1 when no port
# could be had.
#
# For .rfl tests that spawn servers (`(.sys.exec "test/serve.sh /tmp/x ...")`):
# a fixed port is shared by every suite on the host, so two suites running at
# once connected to, and killed, each other's servers (#1140).  A candidate is
# drawn at random below the Linux ephemeral range; one another process takes
# first makes our server's bind fail and the server exit, and the next one is
# tried.  The listener is checked to be ours, not merely open, so a client never
# reaches another suite's server.  Stop the server with `kill $(cat DIR/pid)`,
# never by pattern.  The server's stderr goes to DIR/server.log.

dir=$1
shift
mkdir -p "$dir" || exit 1

# Whether pid $1 listens on TCP port $2.
owns() {
    if command -v ss >/dev/null 2>&1; then
        ss -Hltnp "sport = :$2" 2>/dev/null | grep -q "pid=$1,"
    elif command -v lsof >/dev/null 2>&1; then
        lsof -nP -a -p "$1" -iTCP:"$2" -sTCP:LISTEN >/dev/null 2>&1
    else
        return 1
    fi
}

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
            exit 0
        fi
        sleep 0.05
    done
    kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
done
exit 1
