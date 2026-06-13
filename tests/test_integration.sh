#!/bin/sh
set -eu

daemon=$1
client=$2
ipc_test=$3
directory=$(mktemp -d /tmp/tsched-integration-XXXXXX)
socket="$directory/tsched.sock"
global="$directory/tsched.conf"
tasks="$directory/tasks.conf"
output="$directory/output.log"
errors="$directory/error.log"

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
startup_jitter_ms=0
local_log_kb=8
local_log_total_kb=8
udp_enabled=0
udp_host=127.0.0.1
udp_port=5514
EOF

cat >"$tasks" <<'EOF'
[task:1]
name=integration
enabled=1
schedule=interval
interval_ms=60000
timeout_ms=5000
step=printf 'output-begin:'
step=printf '%04096d' 0
always_step=printf ':output-end'

[task:4]
name=step-machine
enabled=1
schedule=manual
timeout_ms=5000
step=printf 'normal-one\n'
step=printf 'normal-fail\n'; exit 7
step=printf 'must-be-skipped\n'
always_step=printf 'cleanup-one\n'
always_step=printf 'cleanup-fail\n'; exit 9
always_step=printf 'cleanup-last\n'

[task:5]
name=cleanup-failure
enabled=1
schedule=manual
timeout_ms=5000
step=printf 'normal-success\n'
always_step=printf 'only-failure\n'; exit 9
always_step=printf 'cleanup-after-failure\n'
EOF

cat >>"$tasks" <<EOF

[task:6]
name=background-descendant
enabled=1
schedule=manual
timeout_ms=10000
workdir=$directory
step=(trap '' TERM; sleep 30) & echo \$! > background.pid

[task:7]
name=timeout-process-group
enabled=1
schedule=manual
timeout_ms=100
step=trap '' TERM; sleep 30

[task:8]
name=queue-blocker
enabled=1
schedule=manual
timeout_ms=5000
workdir=$directory
step=sleep 0.5; printf 'first\n' >> queue-order.log

[task:9]
name=queue-second
enabled=1
schedule=manual
timeout_ms=5000
workdir=$directory
step=printf 'second\n' >> queue-order.log

[task:10]
name=queue-third
enabled=1
schedule=manual
timeout_ms=5000
workdir=$directory
step=printf 'third\n' >> queue-order.log

[task:11]
name=queue-cancel
enabled=1
schedule=manual
timeout_ms=5000
workdir=$directory
step=printf 'must-not-run\n' >> queue-order.log

[task:12]
name=redirected-background
enabled=1
schedule=manual
timeout_ms=10000
workdir=$directory
step=(sleep 30 >/dev/null 2>&1) & echo \$! > redirected-background.pid

[task:13]
name=cancel-must-not-retry
enabled=1
schedule=manual
timeout_ms=30000
retry_count=2
workdir=$directory
step=printf 'attempt\n' >> cancel-retry.log; trap 'exit 42' TERM; sleep 30

[task:14]
name=disabled-must-not-retry
enabled=1
schedule=manual
timeout_ms=5000
retry_count=2
workdir=$directory
step=printf 'attempt\n' >> disable-retry.log; sleep 0.2; exit 7
EOF

"$daemon" "$global" "$tasks" >"$output" 2>"$errors" &
pid=$!

i=0
while [ ! -S "$socket" ]; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || exit 1
    sleep 0.02
done

"$ipc_test" "$socket"

"$client" -s "$socket" run 1 | grep -q 'OK started'
sleep 0.2
"$client" -s "$socket" list | grep -q "$(printf '1\tintegration\t1\t1\t1')"
grep -q 'output-begin:' "$output"
grep -q ':output-end' "$output"
grep -q ':output-end' "$directory/logs/task-1.log"
[ "$(wc -c <"$directory/logs/task-1.log")" -le 8192 ]
GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=log&id=1' "$client" | grep -q 'output-end'

"$client" -s "$socket" run 4 | grep -q 'OK started'
i=0
while ! "$client" -s "$socket" list |
        awk -F '\t' '$1 == 4 && $5 == 1 && $7 == 7 { found=1 } END { exit !found }'; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
grep -q 'normal-one' "$directory/logs/task-4.log"
grep -q 'normal-fail' "$directory/logs/task-4.log"
! grep -q 'must-be-skipped' "$directory/logs/task-4.log"
grep -q 'step 3/6 normal skipped' "$directory/logs/task-4.log"
grep -q 'cleanup-one' "$directory/logs/task-4.log"
grep -q 'cleanup-fail' "$directory/logs/task-4.log"
grep -q 'cleanup-last' "$directory/logs/task-4.log"
grep -q 'step 6/6 always start' "$directory/logs/task-4.log"

"$client" -s "$socket" run 5 | grep -q 'OK started'
i=0
while ! "$client" -s "$socket" list |
        awk -F '\t' '$1 == 5 && $5 == 1 && $7 == 9 { found=1 } END { exit !found }'; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
grep -q 'normal-success' "$directory/logs/task-5.log"
grep -q 'only-failure' "$directory/logs/task-5.log"
grep -q 'cleanup-after-failure' "$directory/logs/task-5.log"

GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=task&id=4' "$client" | grep -q '"step_count":6'
oversized_name=$(printf '%080d' 0)
GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING="action=task-save&id=15&name=$oversized_name&enabled=1&schedule=manual&interval=0&max_runs=0&timeout=5000&retry=0&workdir=%2Ftmp&command=true" \
"$client" | grep -q 'invalid or oversized query parameter'
GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=task-save&id=4&name=must-not-replace&enabled=1&schedule=manual&interval=0&max_runs=0&timeout=5000&retry=0&workdir=%2Ftmp&command=echo%20lost' \
"$client" | grep -q 'multi-step task is read-only'
grep -q '^always_step=printf.*cleanup-last' "$tasks"

"$client" -s "$socket" run 6 | grep -q 'OK started'
i=0
while [ ! -s "$directory/background.pid" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
background_pid=$(cat "$directory/background.pid")
"$client" -s "$socket" cancel 6 | grep -Eq 'OK terminating|ERR task process unavailable'
"$client" -s "$socket" status | grep -q '^OK '
i=0
while "$client" -s "$socket" list |
        awk -F '\t' '$1 == 6 && $4 == 2 { running=1 } END { exit !running }'; do
    i=$((i + 1))
    [ "$i" -lt 250 ] || exit 1
    sleep 0.02
done
i=0
while kill -0 "$background_pid" 2>/dev/null &&
      [ "$(awk '{ print $3 }' "/proc/$background_pid/stat" 2>/dev/null || echo X)" != "Z" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
"$client" -s "$socket" status | grep -q '^OK '

"$client" -s "$socket" run 7 | grep -q 'OK started'
i=0
while "$client" -s "$socket" list |
        awk -F '\t' '$1 == 7 && $4 == 2 { running=1 } END { exit !running }'; do
    i=$((i + 1))
    [ "$i" -lt 300 ] || exit 1
    sleep 0.02
done
"$client" -s "$socket" list |
    awk -F '\t' '$1 == 7 && $5 == 1 && $7 == -1 { found=1 } END { exit !found }'
"$client" -s "$socket" status | grep -q '^OK '

"$client" -s "$socket" run 8 | grep -q 'OK started'
"$client" -s "$socket" run 9 | grep -q 'OK queued'
"$client" -s "$socket" run 10 | grep -q 'OK queued'
"$client" -s "$socket" status | grep -q 'running=1 waiting=.* queued=2'
"$client" -s "$socket" list |
    awk -F '\t' '$1 == 9 && $4 == 4 { found=1 } END { exit !found }'
i=0
while ! "$client" -s "$socket" list |
        awk -F '\t' '$1 == 10 && $5 == 1 { found=1 } END { exit !found }'; do
    i=$((i + 1))
    [ "$i" -lt 150 ] || exit 1
    sleep 0.02
done
[ "$(cat "$directory/queue-order.log")" = "$(printf 'first\nsecond\nthird')" ]
"$client" -s "$socket" status | grep -q 'queued=0'

"$client" -s "$socket" run 8 | grep -q 'OK started'
"$client" -s "$socket" run 11 | grep -q 'OK queued'
"$client" -s "$socket" cancel 11 | grep -q 'OK canceled'
"$client" -s "$socket" status | grep -q 'queued=0'
i=0
while "$client" -s "$socket" list |
        awk -F '\t' '$1 == 8 && $4 == 2 { running=1 } END { exit !running }'; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
! grep -q 'must-not-run' "$directory/queue-order.log"

"$client" -s "$socket" run 12 | grep -q 'OK started'
i=0
while [ ! -s "$directory/redirected-background.pid" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
redirected_background_pid=$(cat "$directory/redirected-background.pid")
i=0
while "$client" -s "$socket" list |
        awk -F '\t' '$1 == 12 && $4 == 2 { running=1 } END { exit !running }'; do
    i=$((i + 1))
    [ "$i" -lt 250 ] || exit 1
    sleep 0.02
done
! kill -0 "$redirected_background_pid" 2>/dev/null ||
    [ "$(awk '{ print $3 }' "/proc/$redirected_background_pid/stat" \
        2>/dev/null || echo Z)" = "Z" ]

"$client" -s "$socket" run 13 | grep -q 'OK started'
i=0
while [ ! -s "$directory/cancel-retry.log" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
"$client" -s "$socket" cancel 13 | grep -Eq 'OK terminating|ERR task process unavailable'
i=0
while "$client" -s "$socket" list |
        awk -F '\t' '$1 == 13 && $4 == 2 { running=1 } END { exit !running }'; do
    i=$((i + 1))
    [ "$i" -lt 250 ] || exit 1
    sleep 0.02
done
sleep 1.2
[ "$(wc -l <"$directory/cancel-retry.log")" -eq 1 ]

"$client" -s "$socket" run 14 | grep -q 'OK started'
"$client" -s "$socket" disable 14 | grep -q 'OK saved'
i=0
while "$client" -s "$socket" list |
        awk -F '\t' '$1 == 14 && $4 == 2 { running=1 } END { exit !running }'; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
sleep 1.2
[ "$(wc -l <"$directory/disable-retry.log")" -eq 1 ]

GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=task-save&id=2&name=web-task&enabled=0&schedule=manual&interval=0&max_runs=0&timeout=5000&retry=0&workdir=%2Ftmp&command=printf%20web-created' \
"$client" | grep -q 'OK saved'
grep -q '^checksum=' "$tasks"
"$client" -s "$socket" enable 2 | grep -q 'OK saved'
GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=task&id=2' "$client" | grep -q '"name":"web-task"'
"$client" -s "$socket" run 2 | grep -q 'OK started'
sleep 0.1
grep -q 'web-created' "$directory/logs/task-2.log"
[ "$(find "$directory/logs" -type f -name 'task-*.log' -exec wc -c {} + |
     awk 'END { print $1 }')" -le 8192 ]
"$client" -s "$socket" delete 2 | grep -q 'OK deleted'

GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=task-save&id=3&name=cancel-task&enabled=1&schedule=manual&interval=0&max_runs=0&timeout=30000&retry=0&workdir=%2Ftmp&command=sleep%2010' \
"$client" | grep -q 'OK saved'
"$client" -s "$socket" run 3 | grep -q 'OK started'
"$client" -s "$socket" disable 3 | grep -q 'OK saved'
"$client" -s "$socket" cancel 3 | grep -q 'OK terminating'
i=0
while ! "$client" -s "$socket" delete 3 | grep -q 'OK deleted'; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || exit 1
    sleep 0.02
done

GATEWAY_INTERFACE=CGI/1.1 TSCHED_SOCKET="$socket" \
QUERY_STRING='action=config-save&enabled=1&host=127.0.0.1&port=5515' \
"$client" | grep -q 'OK saved'
grep -q '^udp_enabled=1$' "$global"
grep -q '^udp_port=5515$' "$global"

"$client" -s "$socket" run 6 | grep -q 'OK started'
i=0
previous_background_pid=$background_pid
while [ ! -s "$directory/background.pid" ] ||
      [ "$(cat "$directory/background.pid")" = "$previous_background_pid" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
shutdown_background_pid=$(cat "$directory/background.pid")
"$client" -s "$socket" stop | grep -q 'OK stopping'
wait "$pid"
pid=
i=0
while kill -0 "$shutdown_background_pid" 2>/dev/null &&
      [ "$(awk '{ print $3 }' "/proc/$shutdown_background_pid/stat" 2>/dev/null ||
            echo X)" != "Z" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || exit 1
    sleep 0.02
done
