#!/bin/sh
set -eu

daemon=$1
client=$2
directory=$(mktemp -d /tmp/tsched-capacity-XXXXXX)
socket="$directory/tsched.sock"
global="$directory/tsched.conf"
tasks="$directory/tasks.conf"

cleanup() {
    if [ -n "${pid:-}" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$directory"
}
trap cleanup EXIT

cat >"$global" <<EOF
socket_path=$socket
task_file=$tasks
log_dir=$directory/logs
max_running=1
local_log_kb=0
local_log_total_kb=0
udp_enabled=0
EOF

: >"$tasks"
i=1
while [ "$i" -le 256 ]; do
    printf '[task:%u]\nname=task-%03u-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\nenabled=1\nschedule=manual\nstep=true\n\n' \
        "$i" "$i" >>"$tasks"
    i=$((i + 1))
done

"$daemon" "$global" "$tasks" >/dev/null 2>"$directory/error.log" &
pid=$!
i=0
while [ ! -S "$socket" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done

"$client" -s "$socket" status | grep -q 'tasks=256'
[ "$("$client" -s "$socket" list | tail -n +2 | wc -l)" -eq 256 ]
"$client" -s "$socket" list |
    awk -F '\t' '$1 == 256 && $2 ~ /^task-256-/ { found=1 } END { exit !found }'

"$client" -s "$socket" stop | grep -q 'OK stopping'
wait "$pid"
pid=
