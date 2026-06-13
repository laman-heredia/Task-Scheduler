# TSched

TSched is a small C99 task scheduler for resource-constrained OpenWrt devices.
It uses a single `epoll` event loop, `timerfd`, `signalfd`, child process
groups, an atomic text configuration, a Unix domain control socket, and
optional non-blocking UDP logs.

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

Normal steps are joined with `&&`. An `always_step` is joined with `;`, making
it suitable for teardown. Commands execute with `/bin/sh -c`, matching the
internal development and test-oriented deployment model.

## Limits

V1 supports manual and fixed-delay interval tasks. Cron, uploads, persistent
run history, and DAG workflows are intentionally deferred. `tschedctl` detects
the CGI environment and exposes the task list and run/cancel operations to
uHTTPd. Set `TSCHED_SOCKET` in the CGI environment when using a non-default
socket path.
