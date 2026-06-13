#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *trim(char *value)
{
    char *end;
    while (isspace((unsigned char)*value))
        ++value;
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return value;
}

static void copy_value(char *destination, size_t size, const char *value)
{
    if (size == 0)
        return;
    snprintf(destination, size, "%s", value);
}

void tsched_config_defaults(struct tsched_config *config)
{
    /* 默认值优先选择 OpenWrt 上通常位于 tmpfs 的 /tmp，减少 Flash 写入。 */
    memset(config, 0, sizeof(*config));
    copy_value(config->socket_path, sizeof(config->socket_path), "/tmp/tsched.sock");
    copy_value(config->task_file, sizeof(config->task_file), "/etc/tsched/tasks.conf");
    copy_value(config->log_dir, sizeof(config->log_dir), "/tmp/tsched/logs");
    config->max_running = 16;
    config->startup_jitter_ms = 5000;
}

static int load_global(struct tsched_config *config, const char *path)
{
    FILE *file = fopen(path, "r");
    char line[1200];
    if (!file)
        return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof(line), file)) {
        char *equals;
        char *key = trim(line);
        char *value;
        /* 当前全局配置不依赖 section 名，便于保持解析器足够小。 */
        if (*key == '#' || *key == ';' || *key == '[' || *key == '\0')
            continue;
        equals = strchr(key, '=');
        if (!equals)
            continue;
        *equals = '\0';
        value = trim(equals + 1);
        key = trim(key);
        if (!strcmp(key, "socket_path"))
            copy_value(config->socket_path, sizeof(config->socket_path), value);
        else if (!strcmp(key, "task_file"))
            copy_value(config->task_file, sizeof(config->task_file), value);
        else if (!strcmp(key, "log_dir"))
            copy_value(config->log_dir, sizeof(config->log_dir), value);
        else if (!strcmp(key, "udp_host"))
            copy_value(config->udp_host, sizeof(config->udp_host), value);
        else if (!strcmp(key, "udp_port"))
            config->udp_port = (uint16_t)strtoul(value, NULL, 10);
        else if (!strcmp(key, "udp_enabled"))
            config->udp_enabled = atoi(value) != 0;
        else if (!strcmp(key, "max_running"))
            config->max_running = (uint32_t)strtoul(value, NULL, 10);
        else if (!strcmp(key, "startup_jitter_ms"))
            config->startup_jitter_ms = (uint32_t)strtoul(value, NULL, 10);
    }
    fclose(file);
    return 0;
}

static int parse_task_header(const char *header, uint32_t *id)
{
    char tail;
    return sscanf(header, "[task:%u]%c", id, &tail) == 1;
}

static int load_tasks(struct tsched_config *config, const char *path,
                      char *error, size_t error_size)
{
    FILE *file = fopen(path, "r");
    struct tsched_task *task = NULL;
    char line[1200];
    unsigned int line_number = 0;
    if (!file) {
        snprintf(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line);
        char *equals;
        char *value;
        ++line_number;
        if (*key == '#' || *key == ';' || *key == '\0')
            continue;
        if (*key == '[') {
            uint32_t id;
            if (!parse_task_header(key, &id)) {
                task = NULL;
                continue;
            }
            if (config->task_count >= TSCHED_MAX_TASKS) {
                snprintf(error, error_size, "too many tasks at line %u", line_number);
                fclose(file);
                return -1;
            }
            task = &config->tasks[config->task_count++];
            /* 每个新 section 都从安全默认值开始，避免继承前一任务字段。 */
            memset(task, 0, sizeof(*task));
            task->id = id;
            task->timeout_ms = 30000;
            task->output_fd = -1;
            continue;
        }
        if (!task)
            continue;
        equals = strchr(key, '=');
        if (!equals)
            continue;
        *equals = '\0';
        value = trim(equals + 1);
        key = trim(key);
        if (!strcmp(key, "name"))
            copy_value(task->name, sizeof(task->name), value);
        else if (!strcmp(key, "enabled"))
            task->enabled = atoi(value) != 0;
        else if (!strcmp(key, "schedule"))
            task->schedule = !strcmp(value, "interval") ? TSCHED_INTERVAL : TSCHED_MANUAL;
        else if (!strcmp(key, "interval_ms"))
            task->interval_ms = strtoull(value, NULL, 10);
        else if (!strcmp(key, "max_runs"))
            task->max_runs = (uint32_t)strtoul(value, NULL, 10);
        else if (!strcmp(key, "timeout_ms"))
            task->timeout_ms = (uint32_t)strtoul(value, NULL, 10);
        else if (!strcmp(key, "retry_count"))
            task->retry_count = (uint32_t)strtoul(value, NULL, 10);
        else if (!strcmp(key, "workdir"))
            copy_value(task->workdir, sizeof(task->workdir), value);
        else if ((!strcmp(key, "step") || !strcmp(key, "always_step")) &&
                 task->step_count < TSCHED_MAX_STEPS) {
            struct tsched_step *step = &task->steps[task->step_count++];
            copy_value(step->command, sizeof(step->command), value);
            step->always_run = !strcmp(key, "always_step");
        }
    }
    fclose(file);
    return 0;
}

int tsched_config_load(struct tsched_config *config, const char *global_path,
                       const char *task_path, char *error, size_t error_size)
{
    tsched_config_defaults(config);
    if (load_global(config, global_path) != 0) {
        snprintf(error, error_size, "%s: %s", global_path, strerror(errno));
        return -1;
    }
    if (!task_path)
        task_path = config->task_file;
    copy_value(config->task_file, sizeof(config->task_file), task_path);
    return load_tasks(config, task_path, error, error_size);
}

int tsched_config_save_tasks(const struct tsched_config *config,
                             const char *path, char *error, size_t error_size)
{
    char temporary[TSCHED_PATH_LEN + 8];
    FILE *file;
    size_t i, j;
    int fd;
    /*
     * 先完整写入同目录临时文件，再执行原子 rename。掉电发生在 rename
     * 之前时旧文件仍然可用；发生在之后时读取到的是完整新文件。
     */
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    file = fopen(temporary, "w");
    if (!file)
        goto fail;
    fprintf(file, "format_version=%d\n\n", TSCHED_CONFIG_VERSION);
    for (i = 0; i < config->task_count; ++i) {
        const struct tsched_task *task = &config->tasks[i];
        fprintf(file, "[task:%u]\nname=%s\nenabled=%d\nschedule=%s\n"
                "interval_ms=%llu\nmax_runs=%u\ntimeout_ms=%u\n"
                "retry_count=%u\nworkdir=%s\n",
                task->id, task->name, task->enabled,
                task->schedule == TSCHED_INTERVAL ? "interval" : "manual",
                (unsigned long long)task->interval_ms, task->max_runs,
                task->timeout_ms, task->retry_count, task->workdir);
        for (j = 0; j < task->step_count; ++j)
            fprintf(file, "%s=%s\n",
                    task->steps[j].always_run ? "always_step" : "step",
                    task->steps[j].command);
        fputc('\n', file);
    }
    if (fflush(file) != 0)
        goto close_fail;
    fd = fileno(file);
    if (fsync(fd) != 0)
        goto close_fail;
    if (fclose(file) != 0)
        goto fail;
    if (rename(temporary, path) != 0)
        goto fail;
    return 0;
close_fail:
    fclose(file);
fail:
    snprintf(error, error_size, "%s: %s", path, strerror(errno));
    unlink(temporary);
    return -1;
}
