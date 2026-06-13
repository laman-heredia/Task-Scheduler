#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define EVENT_TIMER 1U
#define EVENT_SIGNAL 2U
#define EVENT_SERVER 3U
#define EVENT_OUTPUT_BASE (1ULL << 32)
#define EVENT_CLIENT_BASE (2ULL << 32)
#define EVENT_KIND_MASK (3ULL << 32)
#define EVENT_VALUE_MASK 0xffffffffULL

struct client_connection {
    int fd;
    size_t used;
    uint16_t generation;
    char request[TSCHED_IPC_REQUEST_LEN];
};

/*
 * 守护进程全部可变状态集中在一个结构中，由单线程事件循环独占访问。
 * 这样无需互斥锁，也避免线程栈对 OpenWrt 内存的额外消耗。
 */
struct daemon_state {
    struct tsched_config config;
    struct tsched_heap heap;
    int epoll_fd;
    int timer_fd;
    int signal_fd;
    int server_fd;
    int udp_fd;
    struct sockaddr_storage udp_address;
    socklen_t udp_address_len;
    uint32_t running;
    int stopping;
    const char *global_path;
    uint32_t next_run_id;
    /* 固定容量 FIFO；仅保存稳定的任务 ID，避免任务数组移动后指针失效。 */
    uint32_t pending[TSCHED_MAX_TASKS];
    size_t pending_count;
    struct client_connection clients[TSCHED_MAX_CLIENTS];
    size_t client_count;
    size_t log_total_bytes;
};

static int add_event(int epoll_fd, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

static struct tsched_task *find_task(struct daemon_state *state, uint32_t id)
{
    size_t i;
    for (i = 0; i < state->config.task_count; ++i)
        if (state->config.tasks[i].id == id)
            return &state->config.tasks[i];
    return NULL;
}

static struct tsched_task *find_pid(struct daemon_state *state, pid_t pid)
{
    size_t i;
    for (i = 0; i < state->config.task_count; ++i)
        if (state->config.tasks[i].pid == pid)
            return &state->config.tasks[i];
    return NULL;
}

static int remove_pending(struct daemon_state *state, uint32_t id)
{
    size_t i;
    for (i = 0; i < state->pending_count; ++i) {
        if (state->pending[i] == id) {
            struct tsched_task *task = find_task(state, id);
            if (i + 1U < state->pending_count)
                memmove(&state->pending[i], &state->pending[i + 1U],
                        (state->pending_count - i - 1U) *
                        sizeof(state->pending[0]));
            --state->pending_count;
            if (task)
                task->in_pending = 0;
            return 0;
        }
    }
    return -1;
}

static int enqueue_pending(struct daemon_state *state,
                           struct tsched_task *task)
{
    if (task->in_pending)
        return 0;
    if (state->pending_count >= TSCHED_MAX_TASKS)
        return -1;
    state->pending[state->pending_count++] = task->id;
    task->in_pending = 1;
    task->state = TSCHED_PENDING;
    return 0;
}

static int signal_task_group(const struct tsched_task *task, int signal)
{
    /*
     * kill(0, sig) 会向守护进程所在进程组广播信号。所有调用集中在此处，
     * 严格拒绝无效或特殊 PGID，避免状态竞态演变为误杀守护进程。
     */
    if (task->process_group <= 1) {
        errno = ESRCH;
        return -1;
    }
    return kill(-task->process_group, signal);
}

static int task_group_alive(const struct tsched_task *task)
{
    if (task->process_group <= 1)
        return 0;
    if (kill(-task->process_group, 0) == 0)
        return 1;
    return errno == EPERM;
}

static int reserve_online_tasks(struct daemon_state *state)
{
    struct tsched_task *tasks;
    if (state->config.task_capacity == TSCHED_MAX_TASKS)
        return 0;
    tasks = realloc(state->config.tasks,
                    TSCHED_MAX_TASKS * sizeof(*state->config.tasks));
    if (!tasks)
        return -1;
    state->config.tasks = tasks;
    state->config.task_capacity = TSCHED_MAX_TASKS;
    return 0;
}

static void arm_timer(struct daemon_state *state)
{
    struct itimerspec timer;
    struct tsched_task *task = tsched_heap_peek(&state->heap);
    uint64_t now = tsched_monotonic_ms();
    uint64_t delay = task && task->next_run_ms > now ? task->next_run_ms - now : 1;
    memset(&timer, 0, sizeof(timer));
    /*
     * timerfd 仅跟踪堆顶任务；任务数量增加时不会产生逐任务定时器，
     * 空闲时也不需要按固定周期扫描整个任务表。
     */
    if (task) {
        timer.it_value.tv_sec = (time_t)(delay / 1000U);
        timer.it_value.tv_nsec = (long)(delay % 1000U) * 1000000L;
    }
    (void)timerfd_settime(state->timer_fd, 0, &timer, NULL);
}

static void schedule_task(struct daemon_state *state, struct tsched_task *task,
                          uint64_t delay_ms)
{
    (void)remove_pending(state, task->id);
    (void)tsched_heap_remove(&state->heap, task);
    /* 间隔和重试均基于单调时钟，系统时间跳变不会改变等待时长。 */
    task->next_run_ms = tsched_monotonic_ms() + (delay_ms ? delay_ms : 1);
    task->state = TSCHED_WAITING;
    (void)tsched_heap_push(&state->heap, task);
    arm_timer(state);
}

static void open_task_log(struct daemon_state *state, struct tsched_task *task)
{
    struct stat status;
    task->log_fd = -1;
    task->log_bytes = 0;
    task->log_truncated = 0;
    task->last_log_path[0] = '\0';
    if (!state->config.local_log_kb || !state->config.local_log_total_kb)
        return;
    if (snprintf(task->last_log_path, sizeof(task->last_log_path),
                 "%s/task-%u.log", state->config.log_dir, task->id) >=
        (int)sizeof(task->last_log_path)) {
        task->last_log_path[0] = '\0';
        return;
    }
    /*
     * 任务日志采用按任务覆盖模式。打开前先从全局计数扣除旧文件，
     * 避免循环任务每次执行都把同一个文件重复计入总配额。
     */
    if (stat(task->last_log_path, &status) == 0 && status.st_size > 0) {
        size_t old_size = (size_t)status.st_size;
        state->log_total_bytes = old_size < state->log_total_bytes ?
                                 state->log_total_bytes - old_size : 0;
    }
    task->log_fd = open(task->last_log_path,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

static void write_task_log(struct daemon_state *state, struct tsched_task *task,
                           const char *data, size_t size)
{
    size_t limit = (size_t)state->config.local_log_kb * 1024U;
    size_t total_limit = (size_t)state->config.local_log_total_kb * 1024U;
    size_t available;
    size_t total_available;
    if (task->log_fd < 0 || task->log_truncated)
        return;
    available = task->log_bytes < limit ? limit - task->log_bytes : 0;
    total_available = state->log_total_bytes < total_limit ?
                      total_limit - state->log_total_bytes : 0;
    if (available > total_available)
        available = total_available;
    if (size > available)
        size = available;
    if (!size) {
        task->log_truncated = 1;
        return;
    }
    while (size) {
        ssize_t written = write(task->log_fd, data, size);
        if (written > 0) {
            data += written;
            size -= (size_t)written;
            task->log_bytes += (size_t)written;
            state->log_total_bytes += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            task->log_truncated = 1;
            close(task->log_fd);
            task->log_fd = -1;
            break;
        }
    }
}

static void finish_task(struct daemon_state *state, struct tsched_task *task);
static void dispatch_pending(struct daemon_state *state);
static void complete_step(struct daemon_state *state, struct tsched_task *task);

static void log_step_event(struct daemon_state *state, struct tsched_task *task,
                           const char *event, size_t index, int exit_code)
{
    char message[128];
    int length;
    if (exit_code == INT_MIN)
        length = snprintf(message, sizeof(message), "step=%zu type=%s",
                          index + 1U,
                          task->steps[index].always_run ? "always" : "normal");
    else
        length = snprintf(message, sizeof(message),
                          "step=%zu type=%s code=%d", index + 1U,
                          task->steps[index].always_run ? "always" : "normal",
                          exit_code);
    tsched_udp_send(state->udp_fd, &state->udp_address,
                    state->udp_address_len, task, event, message);
    if (!strcmp(event, "step_start") || !strcmp(event, "step_finish") ||
        !strcmp(event, "step_skip")) {
        char marker[96];
        if (!strcmp(event, "step_start"))
            length = snprintf(marker, sizeof(marker),
                              "\n[tsched: step %zu/%zu %s start]\n",
                              index + 1U, task->step_count,
                              task->steps[index].always_run ?
                                  "always" : "normal");
        else if (!strcmp(event, "step_finish"))
            length = snprintf(marker, sizeof(marker),
                              "\n[tsched: step %zu/%zu %s finish code=%d]\n",
                              index + 1U, task->step_count,
                              task->steps[index].always_run ?
                                  "always" : "normal",
                              exit_code);
        else
            length = snprintf(marker, sizeof(marker),
                              "\n[tsched: step %zu/%zu normal skipped]\n",
                              index + 1U, task->step_count);
        if (length > 0) {
            size_t marker_size = (size_t)length < sizeof(marker) ?
                                 (size_t)length : sizeof(marker) - 1U;
            write_task_log(state, task, marker, marker_size);
        }
    }
}

static int spawn_step(struct daemon_state *state, struct tsched_task *task,
                      size_t index)
{
    int pipe_fds[2];
    pid_t pid;
    if (pipe(pipe_fds) != 0) {
        return -1;
    }
    (void)fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
    /*
     * 父进程通过非阻塞管道接收 stdout/stderr。子进程建立独立进程组，
     * 取消或超时时可以一次终止脚本及其派生进程。
     */
    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        close(pipe_fds[0]);
        (void)dup2(pipe_fds[1], STDOUT_FILENO);
        (void)dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        if (task->workdir[0] && chdir(task->workdir) != 0)
            _exit(126);
        execl("/bin/sh", "sh", "-c", task->steps[index].command, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    /*
     * fork 成功后立即登记所有权。即使后续 pipe/epoll 初始化失败，
     * SIGCHLD 仍能将该子进程关联回当前任务。
     */
    task->pid = pid;
    task->process_group = pid;
    task->active_step = index;
    /* 父进程也尝试设置进程组，消除子进程尚未执行 setpgid 的竞态窗口。 */
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH)
        fprintf(stderr, "warning: setpgid(%ld) failed: %s\n",
                (long)pid, strerror(errno));
    if (tsched_set_nonblock_cloexec(pipe_fds[0]) != 0) {
        close(pipe_fds[0]);
        task->output_fd = -1;
        if (!task->run_failed) {
            task->run_failed = 1;
            task->run_exit_code = 127;
        }
        (void)signal_task_group(task, SIGKILL);
        return 0;
    }
    task->output_fd = pipe_fds[0];
    if (add_event(state->epoll_fd, pipe_fds[0], EPOLLIN | EPOLLRDHUP,
                  EVENT_OUTPUT_BASE + task->id) != 0) {
        close(pipe_fds[0]);
        task->output_fd = -1;
        (void)signal_task_group(task, SIGKILL);
    }
    log_step_event(state, task, "step_start", index, INT_MIN);
    return 0;
}

static void run_next_step(struct daemon_state *state, struct tsched_task *task)
{
    while (task->next_step < task->step_count) {
        size_t index = task->next_step++;
        if (task->run_failed && !task->steps[index].always_run) {
            log_step_event(state, task, "step_skip", index,
                           task->run_exit_code);
            continue;
        }
        if (spawn_step(state, task, index) == 0)
            return;
        /*
         * fork/pipe 失败视为步骤失败。继续扫描可确保资源紧张时仍尽量
         * 执行后续 always_step；普通步骤则按失败短路语义跳过。
         */
        if (!task->run_failed) {
            task->run_failed = 1;
            task->run_exit_code = 127;
        }
        log_step_event(state, task, "step_finish", index, 127);
    }
    finish_task(state, task);
}

static int start_task(struct daemon_state *state, struct tsched_task *task)
{
    if (task->state == TSCHED_RUNNING || task->exit_pending ||
        !task->step_count)
        return -1;
    if (state->config.max_running && state->running >= state->config.max_running)
        return -1;
    (void)remove_pending(state, task->id);
    task->state = TSCHED_RUNNING;
    task->started_ms = tsched_monotonic_ms();
    task->last_run_id = ++state->next_run_id;
    task->next_step = 0;
    task->active_step = 0;
    task->run_failed = 0;
    task->run_exit_code = 0;
    task->exit_pending = 0;
    task->terminating = 0;
    task->stop_reason = TSCHED_STOP_NONE;
    task->group_draining = 0;
    task->process_group = 0;
    open_task_log(state, task);
    ++state->running;
    tsched_udp_send(state->udp_fd, &state->udp_address,
                    state->udp_address_len, task, "start", "");
    run_next_step(state, task);
    return 0;
}

static void dispatch_pending(struct daemon_state *state)
{
    if (state->stopping) {
        while (state->pending_count) {
            uint32_t id = state->pending[0];
            struct tsched_task *task;
            (void)remove_pending(state, id);
            task = find_task(state, id);
            if (task)
                task->state = task->enabled ?
                              TSCHED_WAITING : TSCHED_DISABLED;
        }
        return;
    }
    while (state->pending_count &&
           (!state->config.max_running ||
            state->running < state->config.max_running)) {
        uint32_t id = state->pending[0];
        struct tsched_task *task;
        (void)remove_pending(state, id);
        task = find_task(state, id);
        if (!task)
            continue;
        /*
         * 队首任务无法启动时不能阻塞后续任务。配置解析已经保证正常
         * 任务至少有一个步骤，因此这里只处理运行期异常或陈旧队列项。
         */
        if (start_task(state, task) != 0) {
            task->state = task->enabled ? TSCHED_WAITING : TSCHED_DISABLED;
            if (task->enabled && task->schedule == TSCHED_INTERVAL)
                schedule_task(state, task, task->interval_ms);
        }
    }
}

static void finish_task(struct daemon_state *state, struct tsched_task *task)
{
    char message[96];
    int success = !task->run_failed;
    enum tsched_stop_reason stop_reason = task->stop_reason;
    if (task->output_fd >= 0) {
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, task->output_fd, NULL);
        close(task->output_fd);
        task->output_fd = -1;
    }
    task->pid = 0;
    task->process_group = 0;
    task->exit_pending = 0;
    task->terminating = 0;
    task->group_draining = 0;
    task->last_success = success;
    task->last_exit_code = task->run_exit_code;
    if (task->log_fd >= 0) {
        static const char marker[] = "\n[tsched: log truncated]\n";
        if (task->log_truncated) {
            /*
             * 截断标记本身也必须受单任务和全局配额约束，不能为了提示
             * 越过管理员设置的 tmpfs 内存上限。
             */
            task->log_truncated = 0;
            write_task_log(state, task, marker, sizeof(marker) - 1U);
            task->log_truncated = 1;
        }
        close(task->log_fd);
        task->log_fd = -1;
    }
    if (state->running)
        --state->running;
    snprintf(message, sizeof(message), "status=%s code=%d",
             success ? "success" : "failed",
             task->run_exit_code);
    tsched_udp_send(state->udp_fd, &state->udp_address,
                    state->udp_address_len, task, "finish", message);
    /* 重试次数仅保存在内存；掉电重启后任务从干净状态重新开始。 */
    if (!success && task->enabled && !state->stopping &&
        stop_reason != TSCHED_STOP_CANCEL &&
        stop_reason != TSCHED_STOP_SHUTDOWN &&
        task->retries_done < task->retry_count) {
        ++task->retries_done;
        task->stop_reason = TSCHED_STOP_NONE;
        schedule_task(state, task, 1000U);
        dispatch_pending(state);
        return;
    }
    task->stop_reason = TSCHED_STOP_NONE;
    task->retries_done = 0;
    ++task->run_count;
    /*
     * 在任务结束后重新计算下一次触发时间，属于 fixed-delay 调度；
     * 因此长任务不会造成多个过期实例堆积。
     */
    if (task->enabled && task->schedule == TSCHED_INTERVAL &&
        (!task->max_runs || task->run_count < task->max_runs))
        schedule_task(state, task, task->interval_ms);
    else
        task->state = task->enabled ? TSCHED_WAITING : TSCHED_DISABLED;
    dispatch_pending(state);
}

static void complete_step(struct daemon_state *state, struct tsched_task *task)
{
    int status = task->exit_status;
    int success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    size_t index = task->active_step;
    if (task->output_fd >= 0) {
        (void)epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, task->output_fd, NULL);
        close(task->output_fd);
        task->output_fd = -1;
    }
    task->pid = 0;
    task->process_group = 0;
    task->exit_pending = 0;
    task->group_draining = 0;
    log_step_event(state, task, "step_finish", index, exit_code);
    if (!success && !task->run_failed) {
        task->run_failed = 1;
        task->run_exit_code = exit_code;
    }
    /*
     * 显式取消或任务级超时表示立即终止整轮任务，不再启动新步骤。
     * 普通步骤失败则继续扫描并执行所有 always_step。
     */
    if (task->terminating) {
        task->run_failed = 1;
        if (!task->run_exit_code)
            task->run_exit_code = exit_code ? exit_code : -1;
        finish_task(state, task);
        return;
    }
    run_next_step(state, task);
}

static void begin_group_drain(struct tsched_task *task)
{
    if (task->group_draining)
        return;
    (void)signal_task_group(task, SIGTERM);
    task->group_draining = 1;
    task->terminate_at_ms = tsched_monotonic_ms();
}

static void maybe_complete_exited_step(struct daemon_state *state,
                                       struct tsched_task *task)
{
    if (!task->exit_pending)
        return;
    if (task_group_alive(task)) {
        /*
         * 输出管道 EOF 不能证明进程组已经清空：后台后代可能已把
         * stdout/stderr 重定向到 /dev/null。leader 退出后始终检查
         * PGID，确认不存在残留进程后才允许步骤完成。
         */
        begin_group_drain(task);
        return;
    }
    if (task->output_fd >= 0) {
        (void)epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL,
                        task->output_fd, NULL);
        close(task->output_fd);
        task->output_fd = -1;
    }
    complete_step(state, task);
}

static int drain_output(struct daemon_state *state, struct tsched_task *task)
{
    char buffer[768];
    ssize_t count;
    if (task->output_fd < 0)
        return 1;
    while ((count = read(task->output_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[count] = '\0';
        fwrite(buffer, 1, (size_t)count, stdout);
        fflush(stdout);
        write_task_log(state, task, buffer, (size_t)count);
        tsched_udp_send(state->udp_fd, &state->udp_address,
                        state->udp_address_len, task, "output", buffer);
    }
    if (count == 0) {
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, task->output_fd, NULL);
        close(task->output_fd);
        task->output_fd = -1;
        return 1;
    }
    return 0;
}

static void reap_children(struct daemon_state *state)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct tsched_task *task = find_pid(state, pid);
        if (task) {
            task->exit_pending = 1;
            task->exit_status = status;
            task->pid = 0;
            (void)drain_output(state, task);
            maybe_complete_exited_step(state, task);
        }
    }
}

static void check_timeouts(struct daemon_state *state)
{
    uint64_t now = tsched_monotonic_ms();
    size_t i;
    for (i = 0; i < state->config.task_count; ++i) {
        struct tsched_task *task = &state->config.tasks[i];
        /* 对负 PID 发送信号，目标是该任务的整个进程组。 */
        if (task->state != TSCHED_RUNNING || task->process_group <= 1)
            continue;
        if (task->group_draining) {
            if (!task_group_alive(task))
                maybe_complete_exited_step(state, task);
            else if (now - task->terminate_at_ms >= 3000U)
                (void)signal_task_group(task, SIGKILL);
        } else if (!task->terminating && !task->group_draining &&
            task->timeout_ms &&
            now - task->started_ms >= task->timeout_ms) {
            (void)signal_task_group(task, SIGTERM);
            task->terminating = 1;
            task->stop_reason = TSCHED_STOP_TIMEOUT;
            task->terminate_at_ms = now;
        } else if (task->terminating &&
                   now - task->terminate_at_ms >= 3000U) {
            (void)signal_task_group(task, SIGKILL);
        }
    }
}

static void handle_timer(struct daemon_state *state)
{
    uint64_t expirations;
    uint64_t now = tsched_monotonic_ms();
    ssize_t count = read(state->timer_fd, &expirations, sizeof(expirations));
    if (count < 0 && errno != EAGAIN)
        fprintf(stderr, "timer read failed: %s\n", strerror(errno));
    /* 一次取出当前已经到期的全部任务，再重新设置下一次 timerfd。 */
    while (tsched_heap_peek(&state->heap) &&
           tsched_heap_peek(&state->heap)->next_run_ms <= now) {
        struct tsched_task *task = tsched_heap_pop(&state->heap);
        if (!task->enabled)
            continue;
        if (task->state == TSCHED_RUNNING || task->exit_pending)
            continue;
        if (state->config.max_running &&
            state->running >= state->config.max_running) {
            if (enqueue_pending(state, task) != 0)
                schedule_task(state, task, 1000U);
        } else if (start_task(state, task) != 0) {
            schedule_task(state, task, 1000U);
        }
    }
    check_timeouts(state);
    arm_timer(state);
}

static void send_response(int fd, const char *response)
{
    size_t total = strlen(response);
    size_t sent = 0;
    while (sent < total) {
        ssize_t count = send(fd, response + sent, total - sent, MSG_NOSIGNAL);
        if (count > 0)
            sent += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
}

static void reopen_udp_log(struct daemon_state *state)
{
    if (state->udp_fd >= 0)
        close(state->udp_fd);
    state->udp_fd = tsched_udp_open(&state->config, &state->udp_address,
                                    &state->udp_address_len);
}

static void rebuild_heap(struct daemon_state *state)
{
    size_t i;
    uint64_t now = tsched_monotonic_ms();
    tsched_heap_init(&state->heap);
    for (i = 0; i < state->config.task_count; ++i) {
        struct tsched_task *task = &state->config.tasks[i];
        task->in_heap = 0;
        if (task->enabled && task->schedule == TSCHED_INTERVAL &&
            task->state != TSCHED_RUNNING &&
            task->state != TSCHED_PENDING && !task->exit_pending) {
            if (task->next_run_ms <= now)
                task->next_run_ms = now + task->interval_ms;
            task->state = TSCHED_WAITING;
            (void)tsched_heap_push(&state->heap, task);
        }
    }
    arm_timer(state);
}

static int save_tasks(struct daemon_state *state)
{
    char error[256];
    if (tsched_config_save_tasks(&state->config, state->config.task_file,
                                 error, sizeof(error)) != 0) {
        fprintf(stderr, "task save failed: %s\n", error);
        return -1;
    }
    return 0;
}

static int parse_ulong_field(const char *value, unsigned long maximum,
                             unsigned long *result)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno || !*value || *end || parsed > maximum)
        return -1;
    *result = parsed;
    return 0;
}

static int contains_protocol_delimiter(const char *value)
{
    return strpbrk(value, "\t\r\n") != NULL;
}

static int upsert_task(struct daemon_state *state, char *payload)
{
    char *fields[10];
    char *cursor = payload;
    char *save = NULL;
    struct tsched_task *task;
    struct tsched_task old_task;
    struct tsched_task candidate;
    struct tsched_step *new_steps;
    char *new_command;
    int existed;
    unsigned long id, enabled, interval, max_runs, timeout, retry;
    size_t i;
    for (i = 0; i < 10; ++i) {
        fields[i] = strtok_r(i ? NULL : cursor, "\t\r\n", &save);
        if (!fields[i])
            return -1;
    }
    if (strtok_r(NULL, "\t\r\n", &save) ||
        parse_ulong_field(fields[0], UINT32_MAX, &id) ||
        parse_ulong_field(fields[2], 1, &enabled) ||
        parse_ulong_field(fields[4], UINT32_MAX, &interval) ||
        parse_ulong_field(fields[5], UINT32_MAX, &max_runs) ||
        parse_ulong_field(fields[6], UINT32_MAX, &timeout) ||
        parse_ulong_field(fields[7], 1000, &retry) ||
        !fields[1][0] || strlen(fields[1]) >= TSCHED_NAME_LEN ||
        strlen(fields[8]) >= TSCHED_PATH_LEN || !fields[9][0] ||
        strlen(fields[9]) >= TSCHED_COMMAND_LEN ||
        contains_protocol_delimiter(fields[1]) ||
        contains_protocol_delimiter(fields[8]) ||
        contains_protocol_delimiter(fields[9]) ||
        (strcmp(fields[3], "manual") && strcmp(fields[3], "interval")) ||
        (!strcmp(fields[3], "interval") && !interval))
        return -1;
    new_steps = calloc(1, sizeof(*new_steps));
    if (!new_steps)
        return -1;
    new_command = strdup(fields[9]);
    if (!new_command) {
        free(new_steps);
        return -1;
    }
    new_steps[0].command = new_command;
    task = find_task(state, (uint32_t)id);
    existed = task != NULL;
    if (task && (task->state == TSCHED_RUNNING ||
                 task->state == TSCHED_PENDING || task->exit_pending)) {
        free(new_command);
        free(new_steps);
        return -2;
    }
    /*
     * 当前 UPSERT 只能携带一条命令。拒绝覆盖多步骤任务，避免 Web
     * 修改元数据时静默删除其余普通步骤和 always_step。
     */
    if (task && task->step_count > 1U) {
        free(new_command);
        free(new_steps);
        return -3;
    }
    if (!task) {
        if (state->config.task_count >= state->config.task_capacity) {
            free(new_command);
            free(new_steps);
            return -1;
        }
        task = &state->config.tasks[state->config.task_count];
        memset(&old_task, 0, sizeof(old_task));
        ++state->config.task_count;
    } else {
        (void)tsched_heap_remove(&state->heap, task);
        old_task = *task;
    }
    candidate = existed ? old_task : (struct tsched_task){0};
    candidate.id = (uint32_t)id;
    candidate.output_fd = -1;
    candidate.log_fd = -1;
    candidate.in_heap = 0;
    candidate.in_pending = 0;
    candidate.steps = new_steps;
    candidate.step_count = candidate.step_capacity = 1;
    snprintf(candidate.name, sizeof(candidate.name), "%s", fields[1]);
    snprintf(candidate.workdir, sizeof(candidate.workdir), "%s", fields[8]);
    candidate.enabled = (int)enabled;
    candidate.schedule = !strcmp(fields[3], "interval") ?
                         TSCHED_INTERVAL : TSCHED_MANUAL;
    candidate.interval_ms = interval;
    candidate.max_runs = (uint32_t)max_runs;
    candidate.timeout_ms = (uint32_t)timeout;
    candidate.retry_count = (uint32_t)retry;
    candidate.state = candidate.enabled ? TSCHED_WAITING : TSCHED_DISABLED;
    candidate.next_run_ms = tsched_monotonic_ms() + candidate.interval_ms;
    *task = candidate;
    rebuild_heap(state);
    if (save_tasks(state) != 0) {
        free(new_command);
        free(new_steps);
        if (existed)
            *task = old_task;
        else {
            memset(task, 0, sizeof(*task));
            --state->config.task_count;
        }
        rebuild_heap(state);
        return -1;
    }
    if (existed) {
        for (i = 0; i < old_task.step_count; ++i)
            free(old_task.steps[i].command);
        free(old_task.steps);
    }
    return 0;
}

static int delete_task(struct daemon_state *state, uint32_t id)
{
    size_t i, j;
    struct tsched_task removed;
    char log_path[TSCHED_LOG_PATH_LEN];
    for (i = 0; i < state->config.task_count; ++i)
        if (state->config.tasks[i].id == id)
            break;
    if (i == state->config.task_count)
        return -1;
    if (state->config.tasks[i].state == TSCHED_RUNNING ||
        state->config.tasks[i].state == TSCHED_PENDING ||
        state->config.tasks[i].exit_pending)
        return -2;
    removed = state->config.tasks[i];
    (void)tsched_heap_remove(&state->heap, &state->config.tasks[i]);
    if (i + 1U < state->config.task_count)
        memmove(&state->config.tasks[i], &state->config.tasks[i + 1U],
                (state->config.task_count - i - 1U) *
                sizeof(*state->config.tasks));
    --state->config.task_count;
    rebuild_heap(state);
    if (save_tasks(state) != 0) {
        if (i < state->config.task_count)
            memmove(&state->config.tasks[i + 1U], &state->config.tasks[i],
                    (state->config.task_count - i) *
                    sizeof(*state->config.tasks));
        state->config.tasks[i] = removed;
        ++state->config.task_count;
        rebuild_heap(state);
        return -1;
    }
    for (j = 0; j < removed.step_count; ++j)
        free(removed.steps[j].command);
    free(removed.steps);
    if (removed.last_log_path[0])
        snprintf(log_path, sizeof(log_path), "%s", removed.last_log_path);
    else if (snprintf(log_path, sizeof(log_path), "%s/task-%u.log",
                      state->config.log_dir, removed.id) >=
             (int)sizeof(log_path))
        log_path[0] = '\0';
    if (log_path[0]) {
        struct stat status;
        if (stat(log_path, &status) == 0 && status.st_size > 0) {
            size_t size = (size_t)status.st_size;
            state->log_total_bytes = size < state->log_total_bytes ?
                                     state->log_total_bytes - size : 0;
        }
        (void)unlink(log_path);
    }
    return 0;
}

static void send_task_log(int fd, const struct tsched_task *task)
{
    char response[4096];
    ssize_t count;
    int log_fd;
    size_t used;
    off_t size;
    if (!task || !task->last_log_path[0]) {
        send_response(fd, "ERR log not found\n");
        return;
    }
    log_fd = open(task->last_log_path, O_RDONLY | O_CLOEXEC);
    if (log_fd < 0) {
        send_response(fd, "ERR log not found\n");
        return;
    }
    used = (size_t)snprintf(response, sizeof(response), "OK\n");
    size = lseek(log_fd, 0, SEEK_END);
    if (size > (off_t)(sizeof(response) - used - 1U))
        (void)lseek(log_fd, size - (off_t)(sizeof(response) - used - 1U),
                    SEEK_SET);
    else
        (void)lseek(log_fd, 0, SEEK_SET);
    count = read(log_fd, response + used, sizeof(response) - used - 1U);
    close(log_fd);
    if (count < 0) {
        send_response(fd, "ERR log read failed\n");
        return;
    }
    response[used + (size_t)count] = '\0';
    send_response(fd, response);
}

static void handle_command(struct daemon_state *state, int fd, char *command)
{
    /*
     * V1 控制协议是一问一答的短文本协议。每条连接只处理一条命令，
     * 便于 CLI/CGI 使用，同时限制慢客户端长期占用资源。
     */
    uint32_t id;
    if (!strncmp(command, "PING", 4)) {
        send_response(fd, "OK pong\n");
    } else if (!strncmp(command, "STATUS", 6)) {
        char response[128];
        snprintf(response, sizeof(response),
                 "OK tasks=%zu running=%u waiting=%zu queued=%zu\n",
                 state->config.task_count, state->running, state->heap.count,
                 state->pending_count);
        send_response(fd, response);
    } else if (!strncmp(command, "LIST", 4)) {
        char response[TSCHED_IPC_RESPONSE_LEN];
        size_t used = 0, i;
        used += (size_t)snprintf(response + used, sizeof(response) - used, "OK\n");
        for (i = 0; i < state->config.task_count && used < sizeof(response); ++i) {
            struct tsched_task *task = &state->config.tasks[i];
            used += (size_t)snprintf(response + used, sizeof(response) - used,
                                     "%u\t%s\t%d\t%d\t%u\t%u\t%d\t%zu\t%zu\n",
                                     task->id, task->name, task->enabled,
                                     task->state, task->run_count,
                                     task->last_run_id, task->last_exit_code,
                                     task->state == TSCHED_RUNNING ?
                                         task->active_step + 1U : 0U,
                                     task->step_count);
        }
        send_response(fd, response);
    } else if (!strncmp(command, "UPSERT\t", 7)) {
        int result = upsert_task(state, command + 7);
        send_response(fd, result == 0 ? "OK saved\n" :
                      result == -2 ? "ERR task running\n" :
                      result == -3 ? "ERR multi-step task is read-only\n" :
                      "ERR invalid task\n");
    } else if (sscanf(command, "GET %u", &id) == 1) {
        struct tsched_task *task = find_task(state, id);
        char response[1800];
        if (!task)
            send_response(fd, "ERR task not found\n");
        else {
            snprintf(response, sizeof(response),
                     "OK\t%u\t%s\t%d\t%s\t%llu\t%u\t%u\t%u\t%s\t%zu\t%s\n",
                     task->id, task->name, task->enabled,
                     task->schedule == TSCHED_INTERVAL ? "interval" : "manual",
                     (unsigned long long)task->interval_ms, task->max_runs,
                     task->timeout_ms, task->retry_count,
                     task->workdir[0] ? task->workdir : "/",
                     task->step_count,
                     task->step_count ? task->steps[0].command : "");
            send_response(fd, response);
        }
    } else if (sscanf(command, "ENABLE %u", &id) == 1) {
        struct tsched_task *task = find_task(state, id);
        unsigned int enabled;
        if (!task || sscanf(command, "ENABLE %u %u", &id, &enabled) != 2 ||
            enabled > 1U)
            send_response(fd, "ERR invalid task\n");
        else {
            int old_enabled = task->enabled;
            task->enabled = (int)enabled;
            if (save_tasks(state) == 0) {
                if (task->state != TSCHED_RUNNING && !task->exit_pending) {
                    if (!enabled) {
                        (void)remove_pending(state, task->id);
                        (void)tsched_heap_remove(&state->heap, task);
                        task->state = TSCHED_DISABLED;
                        arm_timer(state);
                    } else if (task->schedule == TSCHED_INTERVAL) {
                        schedule_task(state, task, task->interval_ms);
                    } else {
                        task->state = TSCHED_WAITING;
                    }
                }
                send_response(fd, "OK saved\n");
            } else {
                task->enabled = old_enabled;
                send_response(fd, "ERR save failed\n");
            }
        }
    } else if (sscanf(command, "DELETE %u", &id) == 1) {
        int result = delete_task(state, id);
        send_response(fd, result == 0 ? "OK deleted\n" :
                      result == -2 ? "ERR task running\n" :
                      "ERR task not found\n");
    } else if (sscanf(command, "LOG %u", &id) == 1) {
        send_task_log(fd, find_task(state, id));
    } else if (!strncmp(command, "CONFIG", 6) &&
               (command[6] == '\0' || isspace((unsigned char)command[6]))) {
        char response[256];
        snprintf(response, sizeof(response), "OK %d\t%s\t%u\n",
                 state->config.udp_enabled,
                 state->config.udp_host[0] ? state->config.udp_host : "-",
                 state->config.udp_port);
        send_response(fd, response);
    } else if (!strncmp(command, "SETUDP ", 7)) {
        unsigned int enabled, port;
        char host[64];
        char error[256];
        if (sscanf(command + 7, "%u %63s %u", &enabled, host, &port) != 3 ||
            enabled > 1U || port > 65535U || (enabled && (!host[0] || !port))) {
            send_response(fd, "ERR invalid UDP configuration\n");
        } else {
            int old_enabled = state->config.udp_enabled;
            uint16_t old_port = state->config.udp_port;
            char old_host[sizeof(state->config.udp_host)];
            memcpy(old_host, state->config.udp_host, sizeof(old_host));
            state->config.udp_enabled = (int)enabled;
            snprintf(state->config.udp_host, sizeof(state->config.udp_host),
                     "%s", host);
            state->config.udp_port = (uint16_t)port;
            if (tsched_config_save_global(&state->config, state->global_path,
                                          error, sizeof(error)) != 0) {
                state->config.udp_enabled = old_enabled;
                state->config.udp_port = old_port;
                memcpy(state->config.udp_host, old_host, sizeof(old_host));
                send_response(fd, "ERR save failed\n");
            } else {
                reopen_udp_log(state);
                send_response(fd, "OK saved\n");
            }
        }
    } else if (sscanf(command, "RUN %u", &id) == 1) {
        struct tsched_task *task = find_task(state, id);
        if (!task)
            send_response(fd, "ERR task not found\n");
        else if (task->state == TSCHED_RUNNING)
            send_response(fd, "ERR task already running\n");
        else if (task->state == TSCHED_PENDING)
            send_response(fd, "ERR task already queued\n");
        else {
            (void)tsched_heap_remove(&state->heap, task);
            arm_timer(state);
            if (state->config.max_running &&
                state->running >= state->config.max_running) {
                if (enqueue_pending(state, task) == 0)
                    send_response(fd, "OK queued\n");
                else
                    send_response(fd, "ERR queue full\n");
            } else if (start_task(state, task) == 0) {
                send_response(fd, "OK started\n");
            } else {
                if (task->enabled && task->schedule == TSCHED_INTERVAL)
                    schedule_task(state, task, task->interval_ms);
                send_response(fd, "ERR start failed\n");
            }
        }
    } else if (sscanf(command, "CANCEL %u", &id) == 1) {
        struct tsched_task *task = find_task(state, id);
        if (task && task->state == TSCHED_PENDING) {
            (void)remove_pending(state, task->id);
            task->state = task->enabled ? TSCHED_WAITING : TSCHED_DISABLED;
            send_response(fd, "OK canceled\n");
        } else if (!task || task->state != TSCHED_RUNNING) {
            send_response(fd, "ERR task not running\n");
        } else {
            if (signal_task_group(task, SIGTERM) != 0)
                send_response(fd, "ERR task process unavailable\n");
            else
                send_response(fd, "OK terminating\n");
            task->terminating = 1;
            task->stop_reason = TSCHED_STOP_CANCEL;
            task->terminate_at_ms = tsched_monotonic_ms();
        }
    } else if (!strncmp(command, "STOP", 4)) {
        state->stopping = 1;
        send_response(fd, "OK stopping\n");
    } else {
        send_response(fd, "ERR unknown command\n");
    }
}

static void close_client(struct daemon_state *state, size_t index)
{
    struct client_connection *client;
    if (index >= TSCHED_MAX_CLIENTS)
        return;
    client = &state->clients[index];
    if (client->fd < 0)
        return;
    (void)epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    client->fd = -1;
    client->used = 0;
    if (state->client_count)
        --state->client_count;
}

static void accept_clients(struct daemon_state *state)
{
    int fd;
    while ((fd = accept4(state->server_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
        size_t index;
        if (state->client_count >= TSCHED_MAX_CLIENTS) {
            static const char busy[] = "ERR server busy\n";
            (void)send(fd, busy, sizeof(busy) - 1U, MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        for (index = 0; index < TSCHED_MAX_CLIENTS; ++index)
            if (state->clients[index].fd < 0)
                break;
        if (index == TSCHED_MAX_CLIENTS) {
            close(fd);
            continue;
        }
        state->clients[index].fd = fd;
        state->clients[index].used = 0;
        ++state->clients[index].generation;
        if (!state->clients[index].generation)
            ++state->clients[index].generation;
        if (add_event(state->epoll_fd, fd, EPOLLIN | EPOLLRDHUP,
                      EVENT_CLIENT_BASE +
                      ((uint32_t)state->clients[index].generation << 16U) +
                      (uint32_t)index) != 0) {
            state->clients[index].fd = -1;
            close(fd);
            continue;
        }
        ++state->client_count;
    }
}

static void handle_client(struct daemon_state *state, uint32_t token,
                          uint32_t events)
{
    size_t index = token & 0xffffU;
    uint16_t generation = (uint16_t)(token >> 16U);
    struct client_connection *client;
    if (index >= TSCHED_MAX_CLIENTS)
        return;
    client = &state->clients[index];
    /*
     * epoll_wait 已返回的事件可能在关闭连接后仍留在当前批次。generation
     * 防止槽位快速复用时，旧事件误关闭新接受的客户端。
     */
    if (client->fd < 0 || client->generation != generation)
        return;
    while (client->used < sizeof(client->request) - 1U) {
        ssize_t count = recv(client->fd, client->request + client->used,
                             sizeof(client->request) - 1U - client->used, 0);
        if (count > 0) {
            char *newline;
            client->used += (size_t)count;
            client->request[client->used] = '\0';
            newline = memchr(client->request, '\n', client->used);
            if (newline) {
                *newline = '\0';
                if (newline > client->request && newline[-1] == '\r')
                    newline[-1] = '\0';
                handle_command(state, client->fd, client->request);
                close_client(state, index);
                return;
            }
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (count == 0 && client->used) {
            client->request[client->used] = '\0';
            handle_command(state, client->fd, client->request);
        }
        close_client(state, index);
        return;
    }
    if (client->used == sizeof(client->request) - 1U) {
        send_response(client->fd, "ERR request too long\n");
        close_client(state, index);
        return;
    }
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
        close_client(state, index);
}

static void initialize_log_usage(struct daemon_state *state)
{
    DIR *directory;
    struct dirent *entry;
    state->log_total_bytes = 0;
    directory = opendir(state->config.log_dir);
    if (!directory)
        return;
    while ((entry = readdir(directory)) != NULL) {
        char path[TSCHED_LOG_PATH_LEN];
        struct stat status;
        size_t length = strlen(entry->d_name);
        if (length < 10U || strncmp(entry->d_name, "task-", 5) != 0 ||
            strcmp(entry->d_name + length - 4U, ".log") != 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", state->config.log_dir,
                     entry->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &status) == 0 && status.st_size > 0 &&
            S_ISREG(status.st_mode)) {
            size_t size = (size_t)status.st_size;
            if (SIZE_MAX - state->log_total_bytes < size) {
                state->log_total_bytes = SIZE_MAX;
                break;
            }
            state->log_total_bytes += size;
        }
    }
    closedir(directory);
}

static void handle_output(struct daemon_state *state, uint32_t task_id)
{
    struct tsched_task *task = find_task(state, task_id);
    if (!task || task->output_fd < 0)
        return;
    (void)drain_output(state, task);
    maybe_complete_exited_step(state, task);
}

static int setup_server(struct daemon_state *state)
{
    struct sockaddr_un address;
    state->server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (state->server_fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(state->config.socket_path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, state->config.socket_path,
           strlen(state->config.socket_path) + 1);
    unlink(address.sun_path);
    if (bind(state->server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(state->server_fd, TSCHED_MAX_CLIENTS) != 0)
        return -1;
    return add_event(state->epoll_fd, state->server_fd, EPOLLIN, EVENT_SERVER);
}

static int setup_events(struct daemon_state *state)
{
    sigset_t mask;
    state->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    state->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (state->epoll_fd < 0 || state->timer_fd < 0)
        return -1;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    /*
     * 先阻塞信号，再通过 signalfd 纳入 epoll，避免异步信号处理函数
     * 与主循环同时修改任务状态。
     */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        return -1;
    state->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (state->signal_fd < 0)
        return -1;
    if (add_event(state->epoll_fd, state->timer_fd, EPOLLIN, EVENT_TIMER) != 0 ||
        add_event(state->epoll_fd, state->signal_fd, EPOLLIN, EVENT_SIGNAL) != 0)
        return -1;
    return setup_server(state);
}

static void initialize_schedule(struct daemon_state *state)
{
    size_t i;
    tsched_heap_init(&state->heap);
    for (i = 0; i < state->config.task_count; ++i) {
        struct tsched_task *task = &state->config.tasks[i];
        task->in_pending = 0;
        task->state = task->enabled ? TSCHED_WAITING : TSCHED_DISABLED;
        /*
         * 重启后不恢复历史轮次，也不追赶掉电期间错过的执行。
         * 启用的循环任务等待完整间隔，并按任务 ID 添加确定性抖动。
         */
        if (task->enabled && task->schedule == TSCHED_INTERVAL && task->interval_ms)
            schedule_task(state, task, task->interval_ms +
                          (state->config.startup_jitter_ms ?
                           task->id % state->config.startup_jitter_ms : 0));
    }
}

static void shutdown_tasks(struct daemon_state *state)
{
    uint64_t deadline = tsched_monotonic_ms() + 3000U;
    size_t i;
    for (i = 0; i < state->config.task_count; ++i) {
        struct tsched_task *task = &state->config.tasks[i];
        if (task->process_group > 1) {
            task->terminating = 1;
            task->stop_reason = TSCHED_STOP_SHUTDOWN;
            (void)signal_task_group(task, SIGTERM);
        }
    }
    while (state->running && tsched_monotonic_ms() < deadline) {
        for (i = 0; i < state->config.task_count; ++i)
            if (state->config.tasks[i].output_fd >= 0)
                (void)drain_output(state, &state->config.tasks[i]);
        reap_children(state);
        usleep(10000);
    }
    for (i = 0; i < state->config.task_count; ++i) {
        struct tsched_task *task = &state->config.tasks[i];
        if (task->process_group > 1)
            (void)signal_task_group(task, SIGKILL);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char **argv)
{
    const char *global_path = argc > 1 ? argv[1] : "/etc/tsched/tsched.conf";
    const char *task_path = argc > 2 ? argv[2] : NULL;
    struct daemon_state state;
    struct epoll_event events[32];
    char error[256];
    memset(&state, 0, sizeof(state));
    {
        size_t i;
        for (i = 0; i < TSCHED_MAX_CLIENTS; ++i)
            state.clients[i].fd = -1;
    }
    state.server_fd = state.timer_fd = state.signal_fd = state.epoll_fd = -1;
    state.udp_fd = -1;
    state.global_path = global_path;
    /*
     * 接管任务 Shell 遗留的孤儿后代，确保 SIGKILL 后的僵尸由守护
     * 进程回收，而不是依赖设备上的 PID 1 及时 wait。Linux 4.9
     * 已支持 PR_SET_CHILD_SUBREAPER。
     */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "cannot enable child subreaper: %s\n",
                strerror(errno));
        return 1;
    }
    if (tsched_config_load(&state.config, global_path, task_path,
                           error, sizeof(error)) != 0) {
        fprintf(stderr, "configuration error: %s\n", error);
        return 1;
    }
    if (tsched_mkdir_p(state.config.log_dir) != 0)
        fprintf(stderr, "warning: cannot create log directory: %s\n", strerror(errno));
    initialize_log_usage(&state);
    if (reserve_online_tasks(&state) != 0) {
        fprintf(stderr, "cannot reserve task table\n");
        tsched_config_free(&state.config);
        return 1;
    }
    if (setup_events(&state) != 0) {
        fprintf(stderr, "event setup failed: %s\n", strerror(errno));
        if (state.server_fd >= 0)
            close(state.server_fd);
        if (state.signal_fd >= 0)
            close(state.signal_fd);
        if (state.timer_fd >= 0)
            close(state.timer_fd);
        if (state.epoll_fd >= 0)
            close(state.epoll_fd);
        unlink(state.config.socket_path);
        tsched_config_free(&state.config);
        return 1;
    }
    state.udp_fd = tsched_udp_open(&state.config, &state.udp_address,
                                   &state.udp_address_len);
    initialize_schedule(&state);
    fprintf(stderr, "tschedd: loaded %zu tasks, socket %s\n",
            state.config.task_count, state.config.socket_path);
    while (!state.stopping) {
        /* 1 秒超时只用于超时任务兜底检查；定时触发仍由 timerfd 驱动。 */
        int count = epoll_wait(state.epoll_fd, events, 32, 1000);
        int i;
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            break;
        for (i = 0; i < count; ++i) {
            uint64_t value = events[i].data.u64;
            if (value == EVENT_TIMER)
                handle_timer(&state);
            else if (value == EVENT_SIGNAL) {
                struct signalfd_siginfo info;
                while (read(state.signal_fd, &info, sizeof(info)) == sizeof(info)) {
                    if (info.ssi_signo == SIGCHLD)
                        reap_children(&state);
                    else
                        state.stopping = 1;
                }
            } else if (value == EVENT_SERVER)
                accept_clients(&state);
            else if ((value & EVENT_KIND_MASK) == EVENT_CLIENT_BASE)
                handle_client(&state, (uint32_t)(value & EVENT_VALUE_MASK),
                              events[i].events);
            else if ((value & EVENT_KIND_MASK) == EVENT_OUTPUT_BASE)
                handle_output(&state, (uint32_t)(value & EVENT_VALUE_MASK));
        }
        check_timeouts(&state);
        reap_children(&state);
    }
    shutdown_tasks(&state);
    {
        size_t i;
        for (i = 0; i < TSCHED_MAX_CLIENTS; ++i)
            close_client(&state, i);
    }
    unlink(state.config.socket_path);
    tsched_config_free(&state.config);
    return 0;
}
