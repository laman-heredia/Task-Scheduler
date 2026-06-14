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
    char *response;
    size_t response_length;
    size_t response_sent;
    uint64_t deadline_ms;
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

static void close_client(struct daemon_state *state, size_t index);

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

static uint64_t add_milliseconds(uint64_t value, uint64_t increment)
{
    return UINT64_MAX - value < increment ? UINT64_MAX : value + increment;
}

static void arm_timer(struct daemon_state *state)
{
    struct itimerspec timer;
    struct tsched_task *task = tsched_heap_peek(&state->heap);
    uint64_t now = tsched_monotonic_ms();
    uint64_t deadline = task ? task->next_run_ms : UINT64_MAX;
    size_t i;
    memset(&timer, 0, sizeof(timer));
    /*
     * 同一个 timerfd 同时跟踪堆顶调度时间、任务超时和 TERM 宽限期。
     * 这样主循环可以无限期 epoll_wait，毫秒级任务也不依赖固定轮询。
     */
    for (i = 0; i < state->config.task_count; ++i) {
        struct tsched_task *running = &state->config.tasks[i];
        uint64_t candidate;
        if (running->state != TSCHED_RUNNING)
            continue;
        if (running->kill_sent)
            candidate = now + 10U;
        else if (running->terminating || running->group_draining)
            candidate = add_milliseconds(running->terminate_at_ms,
                                         state->config.kill_grace_ms);
        else if (running->timeout_ms)
            candidate = add_milliseconds(running->started_ms,
                                         running->timeout_ms);
        else
            continue;
        if (candidate < deadline)
            deadline = candidate;
    }
    for (i = 0; i < TSCHED_MAX_CLIENTS; ++i) {
        if (state->clients[i].fd >= 0 &&
            state->clients[i].deadline_ms < deadline)
            deadline = state->clients[i].deadline_ms;
    }
    if (deadline != UINT64_MAX) {
        uint64_t delay = deadline > now ? deadline - now : 1U;
        if (delay / 1000U > (uint64_t)INT_MAX) {
            timer.it_value.tv_sec = (time_t)INT_MAX;
            timer.it_value.tv_nsec = 0;
        } else {
            timer.it_value.tv_sec = (time_t)(delay / 1000U);
            timer.it_value.tv_nsec = (long)(delay % 1000U) * 1000000L;
        }
    }
    (void)timerfd_settime(state->timer_fd, 0, &timer, NULL);
}

static void schedule_task(struct daemon_state *state, struct tsched_task *task,
                          uint64_t delay_ms)
{
    (void)remove_pending(state, task->id);
    (void)tsched_heap_remove(&state->heap, task);
    /* 间隔和重试均基于单调时钟，系统时间跳变不会改变等待时长。 */
    task->next_run_ms = add_milliseconds(tsched_monotonic_ms(),
                                         delay_ms ? delay_ms : 1U);
    task->state = TSCHED_WAITING;
    (void)tsched_heap_push(&state->heap, task);
    arm_timer(state);
}

static int task_run_limit_reached(const struct tsched_task *task)
{
    return task->max_runs && task->run_count >= task->max_runs;
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
        (void)snprintf(message, sizeof(message), "step=%zu type=%s",
                       index + 1U,
                       task->steps[index].always_run ? "always" : "normal");
    else
        (void)snprintf(message, sizeof(message),
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
        sigset_t empty_mask;
        char task_id[16];
        char run_id[16];
        char step_index[32];
        /*
         * 守护进程为 signalfd 阻塞了终止信号；fork 会继承该 mask。
         * 执行用户命令前必须解除阻塞，否则 SIGTERM 只能等到宽限期后
         * 依赖 SIGKILL，任务也无法运行自己的 TERM 清理处理器。
         */
        sigemptyset(&empty_mask);
        (void)sigprocmask(SIG_SETMASK, &empty_mask, NULL);
        (void)setpgid(0, 0);
        close(pipe_fds[0]);
        (void)dup2(pipe_fds[1], STDOUT_FILENO);
        (void)dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        (void)snprintf(task_id, sizeof(task_id), "%u", task->id);
        (void)snprintf(run_id, sizeof(run_id), "%u", task->last_run_id);
        (void)snprintf(step_index, sizeof(step_index), "%zu", index + 1U);
        (void)setenv("TSCHED_TASK_ID", task_id, 1);
        (void)setenv("TSCHED_TASK_NAME", task->name, 1);
        (void)setenv("TSCHED_RUN_ID", run_id, 1);
        (void)setenv("TSCHED_STEP_INDEX", step_index, 1);
        (void)setenv("TSCHED_STEP_TYPE",
                     task->steps[index].always_run ? "always" : "normal", 1);
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
        !task->step_count || task_run_limit_reached(task))
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
    task->kill_sent = 0;
    task->process_group = 0;
    open_task_log(state, task);
    ++state->running;
    tsched_udp_send(state->udp_fd, &state->udp_address,
                    state->udp_address_len, task, "start", "");
    run_next_step(state, task);
    arm_timer(state);
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
        if (task_run_limit_reached(task)) {
            task->state = task->enabled ? TSCHED_WAITING : TSCHED_DISABLED;
            continue;
        }
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
    task->kill_sent = 0;
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
        schedule_task(state, task,
                      state->config.retry_delay_ms ?
                          state->config.retry_delay_ms : 1U);
        task->state = TSCHED_RETRY_WAIT;
        task->stop_reason = TSCHED_STOP_NONE;
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
    task->kill_sent = 0;
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
        arm_timer(state);
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
        if (state->config.mirror_output) {
            size_t offset = 0;
            while (offset < (size_t)count) {
                ssize_t written = write(STDOUT_FILENO, buffer + offset,
                                        (size_t)count - offset);
                if (written > 0)
                    offset += (size_t)written;
                else if (written < 0 && errno == EINTR)
                    continue;
                else
                    break;
            }
        }
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
        if (task->kill_sent) {
            if (!task_group_alive(task))
                maybe_complete_exited_step(state, task);
            continue;
        }
        if (task->group_draining) {
            if (!task_group_alive(task))
                maybe_complete_exited_step(state, task);
            else if (now - task->terminate_at_ms >=
                     state->config.kill_grace_ms)
                task->kill_sent =
                    signal_task_group(task, SIGKILL) == 0 || errno == ESRCH;
        } else if (!task->terminating && !task->group_draining &&
            task->timeout_ms &&
            now - task->started_ms >= task->timeout_ms) {
            (void)signal_task_group(task, SIGTERM);
            task->terminating = 1;
            task->stop_reason = TSCHED_STOP_TIMEOUT;
            task->kill_sent = 0;
            task->terminate_at_ms = now;
        } else if (task->terminating &&
                   now - task->terminate_at_ms >=
                       state->config.kill_grace_ms) {
            task->kill_sent =
                signal_task_group(task, SIGKILL) == 0 || errno == ESRCH;
        }
    }
}

static void check_client_timeouts(struct daemon_state *state)
{
    uint64_t now = tsched_monotonic_ms();
    size_t index;
    for (index = 0; index < TSCHED_MAX_CLIENTS; ++index)
        if (state->clients[index].fd >= 0 &&
            now >= state->clients[index].deadline_ms)
            close_client(state, index);
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
        if (task_run_limit_reached(task)) {
            task->state = TSCHED_WAITING;
            continue;
        }
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
    check_client_timeouts(state);
    arm_timer(state);
}

static struct client_connection *find_client_fd(struct daemon_state *state,
                                                int fd, size_t *index_out)
{
    size_t index;
    for (index = 0; index < TSCHED_MAX_CLIENTS; ++index) {
        if (state->clients[index].fd == fd) {
            if (index_out)
                *index_out = index;
            return &state->clients[index];
        }
    }
    return NULL;
}

static int send_response(struct daemon_state *state, int fd,
                         const char *response)
{
    struct client_connection *client = find_client_fd(state, fd, NULL);
    size_t length;
    char *copy;
    if (!client || client->response)
        return -1;
    length = strlen(response);
    copy = malloc(length + 1U);
    if (!copy)
        return -1;
    memcpy(copy, response, length + 1U);
    client->response = copy;
    client->response_length = length;
    client->response_sent = 0;
    client->deadline_ms = add_milliseconds(tsched_monotonic_ms(), 5000U);
    arm_timer(state);
    return 0;
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
            task->state != TSCHED_PENDING && !task->exit_pending &&
            !task_run_limit_reached(task)) {
            if (task->next_run_ms <= now)
                task->next_run_ms = add_milliseconds(now, task->interval_ms);
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

static int store_task(struct daemon_state *state, uint32_t id,
                      const char *name, int enabled,
                      enum tsched_schedule_type schedule, uint64_t interval,
                      uint32_t max_runs, uint32_t timeout, uint32_t retry,
                      const char *workdir, struct tsched_step *new_steps,
                      size_t step_count)
{
    struct tsched_task *task = find_task(state, id);
    struct tsched_task old_task;
    struct tsched_task candidate;
    int existed = task != NULL;
    size_t i;
    if (task && (task->state == TSCHED_RUNNING ||
                 task->state == TSCHED_PENDING || task->exit_pending))
        return -2;
    if (!task) {
        if (state->config.task_count >= state->config.task_capacity)
            return -1;
        task = &state->config.tasks[state->config.task_count];
        memset(&old_task, 0, sizeof(old_task));
        ++state->config.task_count;
    } else {
        (void)tsched_heap_remove(&state->heap, task);
        old_task = *task;
    }
    candidate = existed ? old_task : (struct tsched_task){0};
    candidate.id = id;
    candidate.output_fd = -1;
    candidate.log_fd = -1;
    candidate.in_heap = 0;
    candidate.in_pending = 0;
    candidate.steps = new_steps;
    candidate.step_count = candidate.step_capacity = step_count;
    snprintf(candidate.name, sizeof(candidate.name), "%s", name);
    snprintf(candidate.workdir, sizeof(candidate.workdir), "%s", workdir);
    candidate.enabled = enabled;
    candidate.schedule = schedule;
    candidate.interval_ms = interval;
    candidate.max_runs = max_runs;
    candidate.timeout_ms = timeout;
    candidate.retry_count = retry;
    candidate.state = candidate.enabled ? TSCHED_WAITING : TSCHED_DISABLED;
    candidate.next_run_ms = add_milliseconds(tsched_monotonic_ms(),
                                              candidate.interval_ms);
    *task = candidate;
    rebuild_heap(state);
    if (save_tasks(state) != 0) {
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

static int upsert_task(struct daemon_state *state, char *payload)
{
    char *fields[10];
    char *cursor = payload;
    char *save = NULL;
    struct tsched_step *new_steps;
    char *new_command;
    int result;
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
    {
        struct tsched_task *existing = find_task(state, (uint32_t)id);
        if (existing && existing->step_count > 1U) {
            free(new_command);
            free(new_steps);
            return -3;
        }
    }
    result = store_task(state, (uint32_t)id, fields[1], (int)enabled,
                        !strcmp(fields[3], "interval") ?
                            TSCHED_INTERVAL : TSCHED_MANUAL,
                        interval, (uint32_t)max_runs, (uint32_t)timeout,
                        (uint32_t)retry, fields[8], new_steps, 1U);
    if (result != 0) {
        free(new_command);
        free(new_steps);
        return result;
    }
    return 0;
}

static int append_blob_steps(char *blob, int always_run,
                             struct tsched_step *steps, size_t *count)
{
    char *cursor = blob;
    while (cursor && *cursor) {
        char *separator = strchr(cursor, '\x1e');
        size_t length;
        if (separator)
            *separator = '\0';
        length = strlen(cursor);
        if (!length || length >= TSCHED_COMMAND_LEN ||
            contains_protocol_delimiter(cursor) ||
            *count >= TSCHED_MAX_STEPS)
            return -1;
        steps[*count].command = strdup(cursor);
        if (!steps[*count].command)
            return -1;
        steps[*count].always_run = always_run;
        ++*count;
        cursor = separator ? separator + 1U : NULL;
    }
    return 0;
}

static int upsert_multi_task(struct daemon_state *state, char *payload)
{
    char *fields[11];
    struct tsched_step *steps;
    unsigned long id, enabled, interval, max_runs, timeout, retry;
    enum tsched_schedule_type schedule;
    size_t count = 0;
    size_t i;
    char *cursor = payload;
    int result;
    for (i = 0; i < 10U; ++i) {
        char *tab = strchr(cursor, '\t');
        if (!tab)
            return -1;
        *tab = '\0';
        fields[i] = cursor;
        cursor = tab + 1U;
    }
    fields[10] = cursor;
    if (parse_ulong_field(fields[0], UINT32_MAX, &id) ||
        parse_ulong_field(fields[2], 1U, &enabled) ||
        parse_ulong_field(fields[4], UINT32_MAX, &interval) ||
        parse_ulong_field(fields[5], UINT32_MAX, &max_runs) ||
        parse_ulong_field(fields[6], UINT32_MAX, &timeout) ||
        parse_ulong_field(fields[7], 1000U, &retry) ||
        !fields[1][0] || strlen(fields[1]) >= TSCHED_NAME_LEN ||
        !fields[8][0] || strlen(fields[8]) >= TSCHED_PATH_LEN ||
        contains_protocol_delimiter(fields[1]) ||
        contains_protocol_delimiter(fields[8]) ||
        (strcmp(fields[3], "manual") && strcmp(fields[3], "interval")) ||
        (!strcmp(fields[3], "interval") && !interval))
        return -1;
    schedule = !strcmp(fields[3], "interval") ?
               TSCHED_INTERVAL : TSCHED_MANUAL;
    steps = calloc(TSCHED_MAX_STEPS, sizeof(*steps));
    if (!steps)
        return -1;
    if (append_blob_steps(fields[9], 0, steps, &count) != 0 ||
        append_blob_steps(fields[10], 1, steps, &count) != 0 || !count) {
        for (i = 0; i < count; ++i)
            free(steps[i].command);
        free(steps);
        return -1;
    }
    result = store_task(state, (uint32_t)id, fields[1], (int)enabled,
                        schedule, interval, (uint32_t)max_runs,
                        (uint32_t)timeout, (uint32_t)retry, fields[8],
                        steps, count);
    if (result != 0) {
        for (i = 0; i < count; ++i)
            free(steps[i].command);
        free(steps);
    }
    return result;
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

static void send_task_log(struct daemon_state *state, int fd,
                          const struct tsched_task *task)
{
    unsigned char raw[4096];
    char response[16384];
    ssize_t count;
    int log_fd;
    size_t used, index;
    off_t size;
    if (!task || !task->last_log_path[0]) {
        send_response(state, fd, "ERR log not found\n");
        return;
    }
    log_fd = open(task->last_log_path, O_RDONLY | O_CLOEXEC);
    if (log_fd < 0) {
        send_response(state, fd, "ERR log not found\n");
        return;
    }
    used = (size_t)snprintf(response, sizeof(response), "OK\n");
    size = lseek(log_fd, 0, SEEK_END);
    if (size > (off_t)sizeof(raw))
        (void)lseek(log_fd, size - (off_t)sizeof(raw), SEEK_SET);
    else
        (void)lseek(log_fd, 0, SEEK_SET);
    count = read(log_fd, raw, sizeof(raw));
    close(log_fd);
    if (count < 0) {
        send_response(state, fd, "ERR log read failed\n");
        return;
    }
    /*
     * IPC 是文本协议。保留常见空白和可打印 ASCII，其他任意字节编码
     * 为 \\xHH，避免 NUL 截断响应或非法 UTF-8 破坏 CGI JSON。
     */
    for (index = 0; index < (size_t)count && used + 5U < sizeof(response);
         ++index) {
        unsigned char byte = raw[index];
        if (byte == '\n' || byte == '\r' || byte == '\t' ||
            (byte >= 0x20U && byte < 0x7fU)) {
            response[used++] = (char)byte;
        } else {
            static const char hex[] = "0123456789abcdef";
            response[used++] = '\\';
            response[used++] = 'x';
            response[used++] = hex[byte >> 4U];
            response[used++] = hex[byte & 0x0fU];
        }
    }
    response[used] = '\0';
    send_response(state, fd, response);
}

static int parse_id_command(const char *command, const char *verb, uint32_t *id)
{
    const char *field;
    size_t verb_length = strlen(verb);
    unsigned long parsed;
    if (strncmp(command, verb, verb_length) || command[verb_length] != ' ')
        return 0;
    field = command + verb_length + 1U;
    if (parse_ulong_field(field, UINT32_MAX, &parsed) != 0)
        return 0;
    *id = (uint32_t)parsed;
    return 1;
}

static int parse_enable_command(const char *command, uint32_t *id, int *enabled)
{
    char extra;
    unsigned long task_id;
    unsigned long value;
    if (sscanf(command, "ENABLE %lu %lu %c",
               &task_id, &value, &extra) != 2 ||
        task_id > UINT32_MAX || value > 1U)
        return 0;
    *id = (uint32_t)task_id;
    *enabled = (int)value;
    return 1;
}

static void handle_command(struct daemon_state *state, int fd, char *command)
{
    /*
     * V1 控制协议是一问一答的短文本协议。每条连接只处理一条命令，
     * 便于 CLI/CGI 使用，同时限制慢客户端长期占用资源。
     */
    uint32_t id;
    if (!strcmp(command, "PING")) {
        send_response(state, fd, "OK pong\n");
    } else if (!strcmp(command, "STATUS")) {
        char response[128];
        snprintf(response, sizeof(response),
                 "OK tasks=%zu running=%u waiting=%zu queued=%zu\n",
                 state->config.task_count, state->running, state->heap.count,
                 state->pending_count);
        send_response(state, fd, response);
    } else if (!strcmp(command, "LIST")) {
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
        send_response(state, fd, response);
    } else if (!strncmp(command, "UPSERTM\t", 8)) {
        int result = upsert_multi_task(state, command + 8);
        send_response(state, fd, result == 0 ? "OK saved\n" :
                      result == -2 ? "ERR task running\n" :
                      "ERR invalid task\n");
    } else if (!strncmp(command, "UPSERT\t", 7)) {
        int result = upsert_task(state, command + 7);
        send_response(state, fd, result == 0 ? "OK saved\n" :
                      result == -2 ? "ERR task running\n" :
                      result == -3 ? "ERR multi-step task is read-only\n" :
                      "ERR invalid task\n");
    } else if (parse_id_command(command, "GET", &id)) {
        struct tsched_task *task = find_task(state, id);
        char response[TSCHED_IPC_RESPONSE_LEN];
        if (!task)
            send_response(state, fd, "ERR task not found\n");
        else {
            size_t used = (size_t)snprintf(
                response, sizeof(response),
                "OK\t%u\t%s\t%d\t%s\t%llu\t%u\t%u\t%u\t%s\t%zu\t",
                task->id, task->name, task->enabled,
                task->schedule == TSCHED_INTERVAL ? "interval" : "manual",
                (unsigned long long)task->interval_ms, task->max_runs,
                task->timeout_ms, task->retry_count,
                task->workdir[0] ? task->workdir : "/", task->step_count);
            size_t step;
            for (step = 0; step < task->step_count && used < sizeof(response);
                 ++step) {
                int count = snprintf(response + used, sizeof(response) - used,
                                     "%s%d:%s", step ? "\x1e" : "",
                                     task->steps[step].always_run,
                                     task->steps[step].command);
                if (count < 0 || (size_t)count >= sizeof(response) - used) {
                    used = sizeof(response);
                    break;
                }
                used += (size_t)count;
            }
            if (used + 2U > sizeof(response))
                send_response(state, fd, "ERR task response too large\n");
            else {
                response[used++] = '\n';
                response[used] = '\0';
                send_response(state, fd, response);
            }
        }
    } else {
        int requested_enabled;
        if (parse_enable_command(command, &id, &requested_enabled)) {
            struct tsched_task *task = find_task(state, id);
            if (!task)
                send_response(state, fd, "ERR invalid task\n");
            else {
                int old_enabled = task->enabled;
                task->enabled = requested_enabled;
                if (save_tasks(state) == 0) {
                    /*
                     * 0 -> 1 明确定义为一次新的测试周期。守护进程重启本来就
                     * 不持久化 run_count；重新启用也重置轮次，方便重复测试。
                     */
                    if (!old_enabled && requested_enabled) {
                        task->run_count = 0;
                        task->retries_done = 0;
                    }
                    if (task->state != TSCHED_RUNNING && !task->exit_pending) {
                        if (!requested_enabled) {
                            (void)remove_pending(state, task->id);
                            (void)tsched_heap_remove(&state->heap, task);
                            task->state = TSCHED_DISABLED;
                            arm_timer(state);
                        } else if (!old_enabled &&
                                   task->schedule == TSCHED_INTERVAL) {
                            schedule_task(state, task, task->interval_ms);
                        } else if (!old_enabled) {
                            task->state = TSCHED_WAITING;
                        }
                    }
                    send_response(state, fd, "OK saved\n");
                } else {
                    task->enabled = old_enabled;
                    send_response(state, fd, "ERR save failed\n");
                }
            }
        } else if (parse_id_command(command, "DELETE", &id)) {
            int result = delete_task(state, id);
            send_response(state, fd, result == 0    ? "OK deleted\n"
                              : result == -2 ? "ERR task running\n"
                                             : "ERR task not found\n");
        } else if (parse_id_command(command, "LOG", &id)) {
            send_task_log(state, fd, find_task(state, id));
        } else if (!strcmp(command, "CONFIG")) {
            char response[256];
            snprintf(response, sizeof(response), "OK %d\t%s\t%u\n",
                     state->config.udp_enabled,
                     state->config.udp_host[0] ? state->config.udp_host : "-",
                     state->config.udp_port);
            send_response(state, fd, response);
        } else if (!strncmp(command, "SETUDP ", 7)) {
            unsigned int enabled, port;
            char host[64];
            char extra;
            char error[256];
            if (sscanf(command + 7, "%u %63s %u %c", &enabled, host, &port,
                       &extra) != 3 ||
                enabled > 1U || port > 65535U ||
                (enabled && (!host[0] || !port))) {
                send_response(state, fd, "ERR invalid UDP configuration\n");
            } else {
                int old_enabled = state->config.udp_enabled;
                uint16_t old_port = state->config.udp_port;
                char old_host[sizeof(state->config.udp_host)];
                memcpy(old_host, state->config.udp_host, sizeof(old_host));
                state->config.udp_enabled = (int)enabled;
                snprintf(state->config.udp_host, sizeof(state->config.udp_host),
                         "%s", host);
                state->config.udp_port = (uint16_t)port;
                if (tsched_config_save_global(&state->config,
                                              state->global_path, error,
                                              sizeof(error)) != 0) {
                    state->config.udp_enabled = old_enabled;
                    state->config.udp_port = old_port;
                    memcpy(state->config.udp_host, old_host, sizeof(old_host));
                    send_response(state, fd, "ERR save failed\n");
                } else {
                    reopen_udp_log(state);
                    send_response(state, fd, "OK saved\n");
                }
            }
        } else if (parse_id_command(command, "RUN", &id)) {
            struct tsched_task *task = find_task(state, id);
            if (!task)
                send_response(state, fd, "ERR task not found\n");
            else if (task_run_limit_reached(task))
                send_response(state, fd, "ERR max runs reached\n");
            else if (task->state == TSCHED_RUNNING)
                send_response(state, fd, "ERR task already running\n");
            else if (task->state == TSCHED_PENDING)
                send_response(state, fd, "ERR task already queued\n");
            else {
                (void)tsched_heap_remove(&state->heap, task);
                arm_timer(state);
                if (state->config.max_running &&
                    state->running >= state->config.max_running) {
                    if (enqueue_pending(state, task) == 0)
                        send_response(state, fd, "OK queued\n");
                    else
                        send_response(state, fd, "ERR queue full\n");
                } else if (start_task(state, task) == 0) {
                    send_response(state, fd, "OK started\n");
                } else {
                    if (task->enabled && task->schedule == TSCHED_INTERVAL)
                        schedule_task(state, task, task->interval_ms);
                    send_response(state, fd, "ERR start failed\n");
                }
            }
        } else if (parse_id_command(command, "CANCEL", &id)) {
            struct tsched_task *task = find_task(state, id);
            if (task && task->state == TSCHED_PENDING) {
                (void)remove_pending(state, task->id);
                task->state = task->enabled ? TSCHED_WAITING : TSCHED_DISABLED;
                send_response(state, fd, "OK canceled\n");
            } else if (!task || task->state != TSCHED_RUNNING) {
                send_response(state, fd, "ERR task not running\n");
            } else {
                if (signal_task_group(task, SIGTERM) != 0)
                    send_response(state, fd, "ERR task process unavailable\n");
                else
                    send_response(state, fd, "OK terminating\n");
                task->terminating = 1;
                task->kill_sent = 0;
                task->stop_reason = TSCHED_STOP_CANCEL;
                task->terminate_at_ms = tsched_monotonic_ms();
                arm_timer(state);
            }
        } else if (!strcmp(command, "STOP")) {
            state->stopping = 1;
            send_response(state, fd, "OK stopping\n");
        } else {
            send_response(state, fd, "ERR unknown command\n");
        }
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
    free(client->response);
    client->response = NULL;
    client->response_length = 0;
    client->response_sent = 0;
    client->deadline_ms = 0;
    client->fd = -1;
    client->used = 0;
    if (state->client_count)
        --state->client_count;
}

static void flush_client_response(struct daemon_state *state, size_t index)
{
    struct client_connection *client;
    if (index >= TSCHED_MAX_CLIENTS)
        return;
    client = &state->clients[index];
    while (client->fd >= 0 && client->response &&
           client->response_sent < client->response_length) {
        ssize_t count = send(client->fd,
                             client->response + client->response_sent,
                             client->response_length - client->response_sent,
                             MSG_NOSIGNAL);
        if (count > 0) {
            client->response_sent += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        close_client(state, index);
        return;
    }
    if (client->fd >= 0 && client->response &&
        client->response_sent == client->response_length)
        close_client(state, index);
}

static void start_client_response(struct daemon_state *state, size_t index)
{
    struct client_connection *client;
    struct epoll_event event;
    if (index >= TSCHED_MAX_CLIENTS)
        return;
    client = &state->clients[index];
    if (client->fd < 0 || !client->response) {
        close_client(state, index);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT | EPOLLRDHUP;
    event.data.u64 = EVENT_CLIENT_BASE +
                     ((uint32_t)client->generation << 16U) +
                     (uint32_t)index;
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD, client->fd, &event) != 0) {
        close_client(state, index);
        return;
    }
    flush_client_response(state, index);
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
        free(state->clients[index].response);
        state->clients[index].response = NULL;
        state->clients[index].response_length = 0;
        state->clients[index].response_sent = 0;
        state->clients[index].deadline_ms =
            add_milliseconds(tsched_monotonic_ms(), 5000U);
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
        arm_timer(state);
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
    if (client->response) {
        if (events & EPOLLOUT)
            flush_client_response(state, index);
        else if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            close_client(state, index);
        return;
    }
    while (client->used < sizeof(client->request) - 1U) {
        ssize_t count = recv(client->fd, client->request + client->used,
                             sizeof(client->request) - 1U - client->used, 0);
        if (count > 0) {
            char *newline;
            client->used += (size_t)count;
            client->deadline_ms =
                add_milliseconds(tsched_monotonic_ms(), 5000U);
            client->request[client->used] = '\0';
            newline = memchr(client->request, '\n', client->used);
            if (newline) {
                *newline = '\0';
                if (newline > client->request && newline[-1] == '\r')
                    newline[-1] = '\0';
                handle_command(state, client->fd, client->request);
                start_client_response(state, index);
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
            start_client_response(state, index);
            return;
        }
        close_client(state, index);
        return;
    }
    if (client->used == sizeof(client->request) - 1U) {
        send_response(state, client->fd, "ERR request too long\n");
        start_client_response(state, index);
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
    if (chmod(address.sun_path, (mode_t)state->config.socket_mode) != 0)
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
            schedule_task(
                state, task,
                add_milliseconds(
                    task->interval_ms,
                    state->config.startup_jitter_ms ?
                        task->id % state->config.startup_jitter_ms : 0U));
    }
}

static void shutdown_tasks(struct daemon_state *state)
{
    uint64_t deadline = add_milliseconds(tsched_monotonic_ms(),
                                         state->config.kill_grace_ms);
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
    if (state.config.mirror_output &&
        tsched_set_nonblock_cloexec(STDOUT_FILENO) != 0)
        fprintf(stderr, "warning: cannot make stdout non-blocking: %s\n",
                strerror(errno));
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
        int count = epoll_wait(state.epoll_fd, events, 32, -1);
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
