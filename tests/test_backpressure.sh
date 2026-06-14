#!/bin/sh
set -eu

daemon=$1
ipc_test=$2
directory=$(mktemp -d /tmp/tsched-backpressure-XXXXXX)
socket="$directory/tsched.sock"
pid=

cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$directory"
}
trap cleanup EXIT

cat >"$directory/tsched.conf" <<EOF
socket_path=$socket
task_file=$directory/tasks.conf
log_dir=$directory/logs
max_running=1
local_log_kb=0
local_log_total_kb=0
udp_enabled=0
EOF

: >"$directory/tasks.conf"
i=1
while [ "$i" -le 256 ]; do
    printf '[task:%u]\nname=task-%03u-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\nenabled=1\nschedule=manual\nstep=true\n\n' \
        "$i" "$i" >>"$directory/tasks.conf"
    i=$((i + 1))
done

"$daemon" "$directory/tsched.conf" "$directory/tasks.conf" \
    >/dev/null 2>"$directory/error.log" &
pid=$!
i=0
while [ ! -S "$socket" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.01
done

"$ipc_test" "$socket" slow-list
"$ipc_test" "$socket" idle-timeout

kill "$pid"
wait "$pid"
pid=
