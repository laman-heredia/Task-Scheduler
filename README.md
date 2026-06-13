# TSched

TSched is a small C99 task scheduler for resource-constrained OpenWrt devices.
It uses a single `epoll` event loop, `timerfd`, `signalfd`, child process
groups, an atomic text configuration, a Unix domain control socket, and
optional non-blocking UDP logs.

The V1 web/CGI interface supports task CRUD, enable/disable, manual execution,
bounded latest-run logs, and persistent UDP log destination settings. Both
task and global configuration use atomic replacement, CRC32 validation, and a
backup fallback. Local logs have per-task and global tmpfs limits.
When the configured concurrency limit is reached, triggered tasks enter a
fixed-capacity FIFO queue instead of polling; completion immediately releases
the next task in trigger order.

中文文档：

- [使用说明](docs/使用说明.md)
- [设计文档](docs/设计文档.md)

## Build

```sh
make
make test
```

## Run

```sh
./build/tschedd ./etc/tsched.conf ./etc/tasks.conf
./build/tschedctl status
./build/tschedctl list
./build/tschedctl run 1
./build/tschedctl cancel 1
./build/tschedctl stop
```

An interval task is rescheduled after it finishes. Its run count is kept only
in memory and resets after a reboot. Enabled interval tasks are restored from
the configuration and resume after one full interval plus deterministic
startup jitter.

## Task configuration

```ini
[task:1]
name=network-test
enabled=1
schedule=interval
interval_ms=60000
max_runs=0
timeout_ms=30000
retry_count=1
workdir=/root/tests
step=./setup.sh
step=./run-test.sh
always_step=./cleanup.sh
```

Each step runs independently through `/bin/sh -c`. After a normal step fails,
later normal steps are skipped while every remaining `always_step` still runs.
The first failure is retained as the task result, so a successful cleanup
cannot hide a failed test. Shell state is not shared between steps. Background
descendants are terminated before the scheduler advances to the next step.

## Limits

V1 supports manual and fixed-delay interval tasks. Cron, uploads, persistent
run history, and DAG workflows are intentionally deferred. `tschedctl` detects
the CGI environment and exposes the task list and run/cancel operations to
uHTTPd. Set `TSCHED_SOCKET` in the CGI environment when using a non-default
socket path.
