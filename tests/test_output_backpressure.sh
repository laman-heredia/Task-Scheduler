#!/bin/sh
set -eu

daemon=$1
client=$2
directory=$(mktemp -d /tmp/tsched-output-pressure-XXXXXX)
socket="$directory/tsched.sock"
fifo="$directory/stdout.fifo"
pid=

cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    exec 9>&- 9<&- || true
    rm -rf "$directory"
}
trap cleanup EXIT

mkfifo "$fifo"
# 以读写方式持有 FIFO，允许 daemon 启动，但故意不消费数据。
exec 9<>"$fifo"

cat >"$directory/tsched.conf" <<EOF
socket_path=$socket
task_file=$directory/tasks.conf
log_dir=$directory/logs
max_running=1
startup_jitter_ms=0
kill_grace_ms=100
retry_delay_ms=10
local_log_kb=4
local_log_total_kb=4
socket_mode=0600
mirror_output=1
udp_enabled=0
EOF

cat >"$directory/tasks.conf" <<'EOF'
[task:1]
name=blocked-daemon-stdout
enabled=1
schedule=manual
timeout_ms=10000
step=dd if=/dev/zero bs=4096 count=512 2>/dev/null | tr '\000' X
EOF

"$daemon" "$directory/tsched.conf" "$directory/tasks.conf" >&9 2>"$directory/error.log" &
pid=$!

i=0
while [ ! -S "$socket" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.01
done

"$client" -s "$socket" run 1 | grep -q 'OK started'
i=0
while ! "$client" -s "$socket" list |
        awk -F '\t' '$1 == 1 && $5 == 1 && $7 == 0 { found=1 } END { exit !found }'; do
    "$client" -s "$socket" status | grep -q '^OK '
    i=$((i + 1))
    [ "$i" -lt 300 ] || exit 1
    sleep 0.01
done

"$client" -s "$socket" stop | grep -q 'OK stopping'
wait "$pid"
pid=
