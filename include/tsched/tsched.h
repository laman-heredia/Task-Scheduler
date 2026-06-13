#ifndef TSCHED_TSCHED_H
#define TSCHED_TSCHED_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * TSched 公共数据结构和模块接口。
 *
 * 项目面向存储资源有限的 OpenWrt 设备，因此所有核心集合都使用明确的
 * 编译期上限，避免任务配置错误导致守护进程无限制分配内存。
 */
#define TSCHED_MAX_TASKS 256
#define TSCHED_MAX_STEPS 16
#define TSCHED_NAME_LEN 64
#define TSCHED_COMMAND_LEN 1024
#define TSCHED_PATH_LEN 256
#define TSCHED_MAX_CLIENTS 16
#define TSCHED_CONFIG_VERSION 1

enum tsched_schedule_type {
    /* 仅通过 CLI、CGI 或其他本地控制端触发。 */
    TSCHED_MANUAL = 0,
    /* 上一轮结束后等待 interval_ms 再触发，属于 fixed-delay 语义。 */
    TSCHED_INTERVAL = 1
};

/* 任务配置和当前运行实例的简化状态。 */
enum tsched_task_state {
    TSCHED_DISABLED = 0,
    TSCHED_WAITING,
    TSCHED_RUNNING,
    TSCHED_RETRY_WAIT
};

struct tsched_step {
    /* V1 中命令统一交给 /bin/sh -c 执行。 */
    char command[TSCHED_COMMAND_LEN];
    /* 非零表示前面步骤失败后仍应执行，通常用于 teardown。 */
    int always_run;
};

struct tsched_task {
    /* 持久化字段：从 tasks.conf 读取，修改时可重新写入配置文件。 */
    uint32_t id;
    char name[TSCHED_NAME_LEN];
    int enabled;
    enum tsched_schedule_type schedule;
    uint64_t interval_ms;
    uint64_t next_run_ms;
    uint32_t max_runs;
    uint32_t run_count;
    uint32_t timeout_ms;
    uint32_t retry_count;

    /* 运行期字段：只存在于内存中，设备重启后重新初始化。 */
    uint32_t retries_done;
    enum tsched_task_state state;
    pid_t pid;
    uint64_t started_ms;
    int output_fd;
    char workdir[TSCHED_PATH_LEN];
    struct tsched_step steps[TSCHED_MAX_STEPS];
    size_t step_count;
};

struct tsched_config {
    /* 守护进程级配置。 */
    char socket_path[TSCHED_PATH_LEN];
    char task_file[TSCHED_PATH_LEN];
    char log_dir[TSCHED_PATH_LEN];
    char udp_host[64];
    uint16_t udp_port;
    int udp_enabled;
    uint32_t max_running;
    uint32_t startup_jitter_ms;

    /* 固定容量任务表，避免在运行期扩容。 */
    struct tsched_task tasks[TSCHED_MAX_TASKS];
    size_t task_count;
};

struct tsched_heap {
    /* 按 next_run_ms 排序的最小堆；items[0] 始终是最近到期任务。 */
    struct tsched_task *items[TSCHED_MAX_TASKS];
    size_t count;
};

/* 加载内置默认配置，不访问文件系统。 */
void tsched_config_defaults(struct tsched_config *config);
/* 依次加载全局配置和任务配置，失败时将原因写入 error。 */
int tsched_config_load(struct tsched_config *config, const char *global_path,
                       const char *task_path, char *error, size_t error_size);
/* 通过“临时文件 + fsync + rename”方式原子保存任务配置。 */
int tsched_config_save_tasks(const struct tsched_config *config,
                             const char *path, char *error, size_t error_size);

/* 最小堆操作，插入和弹出复杂度均为 O(log N)。 */
void tsched_heap_init(struct tsched_heap *heap);
int tsched_heap_push(struct tsched_heap *heap, struct tsched_task *task);
struct tsched_task *tsched_heap_peek(const struct tsched_heap *heap);
struct tsched_task *tsched_heap_pop(struct tsched_heap *heap);

/* 返回 CLOCK_MONOTONIC 毫秒值，供间隔调度和超时计算使用。 */
uint64_t tsched_monotonic_ms(void);
/* 为文件描述符同时设置 O_NONBLOCK 和 FD_CLOEXEC。 */
int tsched_set_nonblock_cloexec(int fd);
/* 创建路径中尚不存在的目录，行为类似受限版 mkdir -p。 */
int tsched_mkdir_p(const char *path);

/* 创建共享的非阻塞 UDP 日志 socket，并解析远端地址。 */
int tsched_udp_open(const struct tsched_config *config,
                    struct sockaddr_storage *address, socklen_t *address_len);
/* 尽力发送一条 UDP 日志；发送失败不会影响任务执行结果。 */
void tsched_udp_send(int fd, const struct sockaddr_storage *address,
                     socklen_t address_len, const struct tsched_task *task,
                     const char *event, const char *message);

#endif
