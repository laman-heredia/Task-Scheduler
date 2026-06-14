#!/bin/sh
set -eu

daemon=$1
client=$2
directory=$(mktemp -d /tmp/tsched-timing-XXXXXX)
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
startup_jitter_ms=0
local_log_kb=0
local_log_total_kb=0
udp_enabled=0
EOF

cat >"$directory/tasks.conf" <<EOF
[task:1]
name=millisecond-loop
enabled=1
schedule=interval
interval_ms=5
max_runs=3
timeout_ms=1000
workdir=$directory
step=printf 'tick\n' >> loop.log

[task:2]
name=millisecond-timeout
enabled=1
schedule=manual
timeout_ms=25
step=trap 'exit 0' TERM; while :; do :; done
EOF

"$daemon" "$directory/tsched.conf" "$directory/tasks.conf" \
    >"$directory/output.log" 2>"$directory/error.log" &
pid=$!

i=0
while [ ! -S "$socket" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.01
done

i=0
while [ ! -f "$directory/loop.log" ] ||
      [ "$(wc -l <"$directory/loop.log")" -lt 3 ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.01
done
[ "$(wc -l <"$directory/loop.log")" -eq 3 ]

# 编辑达到 max_runs 的循环任务不能通过重建调度堆绕过轮次上限。
GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=task-save&id=1&name=millisecond-loop&enabled=1&schedule=interval&interval=5&max_runs=3&timeout=1000&retry=0&workdir='"$directory"'&command=printf%20%27tick%5Cn%27%20%3E%3E%20loop.log' \
"$client" | grep -q 'OK saved'
sleep 0.1
[ "$(wc -l <"$directory/loop.log")" -eq 3 ]

"$client" -s "$socket" run 2 | grep -q 'OK started'
i=0
while "$client" -s "$socket" list |
        awk -F '\t' '$1 == 2 && $4 == 2 { running=1 } END { exit !running }'; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || exit 1
    sleep 0.01
done

"$client" -s "$socket" stop | grep -q 'OK stopping'
wait "$pid"
pid=
