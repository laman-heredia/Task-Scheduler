#ifndef TSCHED_TSCHED_H
#define TSCHED_TSCHED_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * TSched 公共数据结构和模块接口。
 *
 * 项目面向存储资源有限的 OpenWrt 设备，因此核心集合采用按需分配并设置
 * 明确的编译期上限，避免空配置浪费内存或错误配置无限制扩容。
 */
#define TSCHED_MAX_TASKS 256
#define TSCHED_MAX_STEPS 16
#define TSCHED_NAME_LEN 64
#define TSCHED_COMMAND_LEN 1024
#define TSCHED_PATH_LEN 256
#define TSCHED_MAX_CLIENTS 16
#define TSCHED_IPC_REQUEST_LEN 32768
#define TSCHED_IPC_RESPONSE_LEN 49152
#define TSCHED_CONFIG_VERSION 1
#define TSCHED_LOG_PATH_LEN 320

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
    TSCHED_RETRY_WAIT,
    /* 已到期或被手动触发，正在等待并发执行名额。 */
    TSCHED_PENDING
};

enum tsched_stop_reason {
    TSCHED_STOP_NONE = 0,
    TSCHED_STOP_CANCEL,
    TSCHED_STOP_TIMEOUT,
    TSCHED_STOP_SHUTDOWN
};

struct tsched_step {
    /* 每个步骤独立交给 /bin/sh -c 执行。 */
    char *command;
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
    /* leader_pid 用于 waitpid；process_group 在后代清理完成前保持有效。 */
    pid_t pid;
    pid_t process_group;
    uint64_t started_ms;
    int output_fd;
    int exit_pending;
    int exit_status;
    int terminating;
    enum tsched_stop_reason stop_reason;
    int group_draining;
    int kill_sent;
    uint64_t terminate_at_ms;
    /* 逐步骤执行状态；next_step 指向下一条待判断的步骤。 */
    size_t next_step;
    size_t active_step;
    int run_failed;
    int run_exit_code;
    char workdir[TSCHED_PATH_LEN];
    struct tsched_step *steps;
    size_t step_count;
    size_t step_capacity;
    int in_heap;
    int in_pending;
    uint32_t last_run_id;
    int last_exit_code;
    int last_success;
    int log_fd;
    size_t log_bytes;
    int log_truncated;
    char last_log_path[TSCHED_LOG_PATH_LEN];
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
    uint32_t kill_grace_ms;
    uint32_t retry_delay_ms;
    uint32_t local_log_kb;
    /* 所有本地任务日志的总上限；0 表示禁用本地日志。 */
    uint32_t local_log_total_kb;
    uint32_t socket_mode;
    /* 仅以非阻塞、尽力方式复制任务输出到守护进程 stdout。 */
    int mirror_output;

    /* 任务按实际数量分配，避免空配置也预留数 MiB 内存。 */
    struct tsched_task *tasks;
    size_t task_count;
    size_t task_capacity;
};

struct tsched_heap {
    /* 按 next_run_ms 排序的最小堆；items[0] 始终是最近到期任务。 */
    struct tsched_task *items[TSCHED_MAX_TASKS];
    size_t count;
};

/* 加载内置默认配置，不访问文件系统。 */
void tsched_config_defaults(struct tsched_config *config);
void tsched_config_free(struct tsched_config *config);
/* 依次加载全局配置和任务配置，失败时将原因写入 error。 */
int tsched_config_load(struct tsched_config *config, const char *global_path,
                       const char *task_path, char *error, size_t error_size);
/* 通过“临时文件 + fsync + rename”方式原子保存任务配置。 */
int tsched_config_save_tasks(const struct tsched_config *config,
                             const char *path, char *error, size_t error_size);
int tsched_config_save_global(const struct tsched_config *config,
                              const char *path, char *error, size_t error_size);

/* 最小堆操作，插入和弹出复杂度均为 O(log N)。 */
void tsched_heap_init(struct tsched_heap *heap);
int tsched_heap_push(struct tsched_heap *heap, struct tsched_task *task);
int tsched_heap_remove(struct tsched_heap *heap, struct tsched_task *task);
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
