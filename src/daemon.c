#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
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
    /* 间隔和重试均基于单调时钟，系统时间跳变不会改变等待时长。 */
    task->next_run_ms = tsched_monotonic_ms() + (delay_ms ? delay_ms : 1);
    task->state = TSCHED_WAITING;
    (void)tsched_heap_push(&state->heap, task);
    arm_timer(state);
}

static int build_script(const struct tsched_task *task, char *script, size_t size)
{
    size_t used = 0;
    size_t i;
    for (i = 0; i < task->step_count; ++i) {
        int written;
        /*
         * 普通步骤使用 &&，失败后停止后续普通步骤；always_step 使用 ;
         * 接续，主要用于执行清理命令。V2 可将步骤执行改为独立状态机。
         */
        if (i)
            written = snprintf(script + used, size - used,
                               task->steps[i].always_run ? "; " : " && ");
        else
            written = 0;
        if (written < 0 || (size_t)written >= size - used)
            return -1;
        used += (size_t)written;
        written = snprintf(script + used, size - used, "%s", task->steps[i].command);
        if (written < 0 || (size_t)written >= size - used)
            return -1;
        used += (size_t)written;
    }
    return task->step_count ? 0 : -1;
}

static int start_task(struct daemon_state *state, struct tsched_task *task)
{
    int pipe_fds[2];
    char script[TSCHED_COMMAND_LEN * 2];
    pid_t pid;
    if (state->config.max_running && state->running >= state->config.max_running)
        return -1;
    if (build_script(task, script, sizeof(script)) != 0)
        return -1;
    if (pipe(pipe_fds) != 0)
        return -1;
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
        execl("/bin/sh", "sh", "-c", script, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    if (tsched_set_nonblock_cloexec(pipe_fds[0]) != 0) {
        close(pipe_fds[0]);
        kill(-pid, SIGKILL);
        return -1;
    }
    task->pid = pid;
    task->output_fd = pipe_fds[0];
    task->state = TSCHED_RUNNING;
    task->started_ms = tsched_monotonic_ms();
    ++state->running;
    add_event(state->epoll_fd, pipe_fds[0], EPOLLIN | EPOLLRDHUP,
              EVENT_OUTPUT_BASE + task->id);
    tsched_udp_send(state->udp_fd, &state->udp_address,
                    state->udp_address_len, task, "start", "");
    return 0;
}

static void finish_task(struct daemon_state *state, struct tsched_task *task,
                        int status)
{
    char message[96];
    int success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (task->output_fd >= 0) {
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, task->output_fd, NULL);
        close(task->output_fd);
        task->output_fd = -1;
    }
    task->pid = 0;
    if (state->running)
        --state->running;
    snprintf(message, sizeof(message), "status=%s code=%d",
             success ? "success" : "failed",
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    tsched_udp_send(state->udp_fd, &state->udp_address,
                    state->udp_address_len, task, "finish", message);
    /* 重试次数仅保存在内存；掉电重启后任务从干净状态重新开始。 */
    if (!success && task->retries_done < task->retry_count) {
        ++task->retries_done;
        schedule_task(state, task, 1000U);
        return;
    }
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
}

static void reap_children(struct daemon_state *state)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct tsched_task *task = find_pid(state, pid);
        if (task)
            finish_task(state, task, status);
    }
}

static void check_timeouts(struct daemon_state *state)
{
    uint64_t now = tsched_monotonic_ms();
    size_t i;
    for (i = 0; i < state->config.task_count; ++i) {
        struct tsched_task *task = &state->config.tasks[i];
        /* 对负 PID 发送信号，目标是该任务的整个进程组。 */
        if (task->state == TSCHED_RUNNING && task->timeout_ms &&
            now - task->started_ms >= task->timeout_ms)
            (void)kill(-task->pid, SIGKILL);
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
        if (start_task(state, task) != 0)
            schedule_task(state, task, 500U);
    }
    check_timeouts(state);
    arm_timer(state);
}

static void send_response(int fd, const char *response)
{
    (void)send(fd, response, strlen(response), MSG_NOSIGNAL);
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
        snprintf(response, sizeof(response), "OK tasks=%zu running=%u queued=%zu\n",
                 state->config.task_count, state->running, state->heap.count);
        send_response(fd, response);
    } else if (!strncmp(command, "LIST", 4)) {
        char response[4096];
        size_t used = 0, i;
        used += (size_t)snprintf(response + used, sizeof(response) - used, "OK\n");
        for (i = 0; i < state->config.task_count && used < sizeof(response); ++i) {
            struct tsched_task *task = &state->config.tasks[i];
            used += (size_t)snprintf(response + used, sizeof(response) - used,
                                     "%u\t%s\t%d\t%d\t%u\n", task->id, task->name,
                                     task->enabled, task->state, task->run_count);
        }
        send_response(fd, response);
    } else if (sscanf(command, "RUN %u", &id) == 1) {
        struct tsched_task *task = find_task(state, id);
        if (!task)
            send_response(fd, "ERR task not found\n");
        else if (task->state == TSCHED_RUNNING)
            send_response(fd, "ERR task already running\n");
        else if (start_task(state, task) == 0)
            send_response(fd, "OK started\n");
        else
            send_response(fd, "ERR start failed\n");
    } else if (sscanf(command, "CANCEL %u", &id) == 1) {
        struct tsched_task *task = find_task(state, id);
        if (!task || task->state != TSCHED_RUNNING)
            send_response(fd, "ERR task not running\n");
        else {
            (void)kill(-task->pid, SIGTERM);
            send_response(fd, "OK terminating\n");
        }
    } else if (!strncmp(command, "STOP", 4)) {
        state->stopping = 1;
        send_response(fd, "OK stopping\n");
    } else {
        send_response(fd, "ERR unknown command\n");
    }
}

static void accept_clients(struct daemon_state *state)
{
    int fd;
    while ((fd = accept4(state->server_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
        if (add_event(state->epoll_fd, fd, EPOLLIN | EPOLLRDHUP,
              EVENT_CLIENT_BASE + (uint32_t)fd) != 0)
            close(fd);
    }
}

static void handle_client(struct daemon_state *state, int fd)
{
    char command[512];
    ssize_t count = recv(fd, command, sizeof(command) - 1, 0);
    if (count > 0) {
        command[count] = '\0';
        handle_command(state, fd, command);
    }
    epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

static void handle_output(struct daemon_state *state, uint32_t task_id)
{
    struct tsched_task *task = find_task(state, task_id);
    char buffer[768];
    ssize_t count;
    if (!task || task->output_fd < 0)
        return;
    /* 边读取边转发，不在守护进程内存中累计完整任务输出。 */
    while ((count = read(task->output_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[count] = '\0';
        fwrite(buffer, 1, (size_t)count, stdout);
        fflush(stdout);
        tsched_udp_send(state->udp_fd, &state->udp_address,
                        state->udp_address_len, task, "output", buffer);
    }
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

int main(int argc, char **argv)
{
    const char *global_path = argc > 1 ? argv[1] : "/etc/tsched/tsched.conf";
    const char *task_path = argc > 2 ? argv[2] : NULL;
    struct daemon_state state;
    struct epoll_event events[32];
    char error[256];
    memset(&state, 0, sizeof(state));
    state.server_fd = state.timer_fd = state.signal_fd = state.epoll_fd = -1;
    if (tsched_config_load(&state.config, global_path, task_path,
                           error, sizeof(error)) != 0) {
        fprintf(stderr, "configuration error: %s\n", error);
        return 1;
    }
    if (tsched_mkdir_p(state.config.log_dir) != 0)
        fprintf(stderr, "warning: cannot create log directory: %s\n", strerror(errno));
    if (setup_events(&state) != 0) {
        fprintf(stderr, "event setup failed: %s\n", strerror(errno));
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
                handle_client(&state, (int)(value & EVENT_VALUE_MASK));
            else if ((value & EVENT_KIND_MASK) == EVENT_OUTPUT_BASE)
                handle_output(&state, (uint32_t)(value & EVENT_VALUE_MASK));
        }
        check_timeouts(&state);
        reap_children(&state);
    }
    unlink(state.config.socket_path);
    return 0;
}
