#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

static int copy_value(char *destination, size_t size, const char *value)
{
    size_t length = strlen(value);
    if (length >= size)
        return -1;
    memcpy(destination, value, length + 1);
    return 0;
}

static int parse_u64(const char *value, uint64_t maximum, uint64_t *result)
{
    char *end;
    unsigned long long parsed;
    if (!*value || *value == '-')
        return -1;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno || *end || parsed > maximum)
        return -1;
    *result = (uint64_t)parsed;
    return 0;
}

static int parse_u32(const char *value, uint32_t maximum, uint32_t *result)
{
    uint64_t parsed;
    if (parse_u64(value, maximum, &parsed) != 0)
        return -1;
    *result = (uint32_t)parsed;
    return 0;
}

static int parse_bool(const char *value, int *result)
{
    if (!strcmp(value, "1") || !strcmp(value, "true")) {
        *result = 1;
        return 0;
    }
    if (!strcmp(value, "0") || !strcmp(value, "false")) {
        *result = 0;
        return 0;
    }
    return -1;
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *data, size_t size)
{
    size_t i;
    crc = ~crc;
    for (i = 0; i < size; ++i) {
        unsigned int bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static int crc32_fd(int fd, uint32_t *result)
{
    unsigned char buffer[1024];
    off_t offset = 0;
    uint32_t crc = 0;
    ssize_t count;
    while ((count = pread(fd, buffer, sizeof(buffer), offset)) > 0) {
        crc = crc32_update(crc, buffer, (size_t)count);
        offset += count;
    }
    if (count < 0)
        return -1;
    *result = crc;
    return 0;
}

static int verify_checksum(const char *path, char *error, size_t error_size)
{
    FILE *file = fopen(path, "r");
    char line[1200];
    uint32_t crc = 0, expected = 0;
    int found = 0;
    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "checksum=", 9)) {
            char *end;
            unsigned long value;
            errno = 0;
            value = strtoul(line + 9, &end, 16);
            while (*end == '\r' || *end == '\n')
                ++end;
            if (errno || *end || value > UINT32_MAX) {
                snprintf(error, error_size, "%s: invalid checksum", path);
                fclose(file);
                return -1;
            }
            expected = (uint32_t)value;
            found = 1;
        } else {
            crc = crc32_update(crc, (const unsigned char *)line, strlen(line));
        }
    }
    fclose(file);
    if (found && crc != expected) {
        snprintf(error, error_size, "%s: checksum mismatch", path);
        return -1;
    }
    return 0;
}

void tsched_config_defaults(struct tsched_config *config)
{
    memset(config, 0, sizeof(*config));
    (void)copy_value(config->socket_path, sizeof(config->socket_path),
                     "/tmp/tsched.sock");
    (void)copy_value(config->task_file, sizeof(config->task_file),
                     "/etc/tsched/tasks.conf");
    (void)copy_value(config->log_dir, sizeof(config->log_dir),
                     "/tmp/tsched/logs");
    config->max_running = 16;
    config->startup_jitter_ms = 5000;
    config->local_log_kb = 64;
    config->local_log_total_kb = 2048;
}

void tsched_config_free(struct tsched_config *config)
{
    size_t i, j;
    for (i = 0; i < config->task_count; ++i) {
        for (j = 0; j < config->tasks[i].step_count; ++j)
            free(config->tasks[i].steps[j].command);
        free(config->tasks[i].steps);
    }
    free(config->tasks);
    config->tasks = NULL;
    config->task_count = 0;
    config->task_capacity = 0;
}

static int config_error(char *error, size_t size, const char *path,
                        unsigned int line, const char *key)
{
    snprintf(error, size, "%s:%u: invalid value for %s", path, line, key);
    return -1;
}

static int load_global(struct tsched_config *config, const char *path,
                       char *error, size_t error_size)
{
    FILE *file = fopen(path, "r");
    char line[1200];
    unsigned int line_number = 0;
    if (!file)
        return errno == ENOENT ? 0 : -1;
    fclose(file);
    if (verify_checksum(path, error, error_size) != 0)
        return -1;
    file = fopen(path, "r");
    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        char *equals;
        char *key = trim(line);
        char *value;
        uint32_t number;
        ++line_number;
        if (*key == '#' || *key == ';' || *key == '[' || *key == '\0')
            continue;
        equals = strchr(key, '=');
        if (!equals)
            continue;
        *equals = '\0';
        value = trim(equals + 1);
        key = trim(key);
        if (!strcmp(key, "socket_path")) {
            if (copy_value(config->socket_path, sizeof(config->socket_path), value))
                goto invalid;
        } else if (!strcmp(key, "task_file")) {
            if (copy_value(config->task_file, sizeof(config->task_file), value))
                goto invalid;
        } else if (!strcmp(key, "log_dir")) {
            if (copy_value(config->log_dir, sizeof(config->log_dir), value))
                goto invalid;
        } else if (!strcmp(key, "udp_host")) {
            if (copy_value(config->udp_host, sizeof(config->udp_host), value))
                goto invalid;
        } else if (!strcmp(key, "udp_port")) {
            if (parse_u32(value, 65535U, &number))
                goto invalid;
            config->udp_port = (uint16_t)number;
        } else if (!strcmp(key, "udp_enabled")) {
            if (parse_bool(value, &config->udp_enabled))
                goto invalid;
        } else if (!strcmp(key, "max_running")) {
            if (parse_u32(value, 4096U, &config->max_running))
                goto invalid;
        } else if (!strcmp(key, "startup_jitter_ms")) {
            if (parse_u32(value, UINT32_MAX, &config->startup_jitter_ms))
                goto invalid;
        } else if (!strcmp(key, "local_log_kb")) {
            if (parse_u32(value, 16384U, &config->local_log_kb))
                goto invalid;
        } else if (!strcmp(key, "local_log_total_kb")) {
            if (parse_u32(value, 65536U, &config->local_log_total_kb))
                goto invalid;
        }
        continue;
invalid:
        fclose(file);
        return config_error(error, error_size, path, line_number, key);
    }
    if (ferror(file)) {
        snprintf(error, error_size, "%s: read failed", path);
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static struct tsched_task *append_task(struct tsched_config *config)
{
    struct tsched_task *tasks;
    size_t capacity;
    if (config->task_count >= TSCHED_MAX_TASKS)
        return NULL;
    if (config->task_count == config->task_capacity) {
        capacity = config->task_capacity ? config->task_capacity * 2U : 8U;
        if (capacity > TSCHED_MAX_TASKS)
            capacity = TSCHED_MAX_TASKS;
        tasks = realloc(config->tasks, capacity * sizeof(*tasks));
        if (!tasks)
            return NULL;
        config->tasks = tasks;
        config->task_capacity = capacity;
    }
    memset(&config->tasks[config->task_count], 0, sizeof(*config->tasks));
    config->tasks[config->task_count].timeout_ms = 30000;
    config->tasks[config->task_count].output_fd = -1;
    config->tasks[config->task_count].log_fd = -1;
    return &config->tasks[config->task_count++];
}

static int append_step(struct tsched_task *task, const char *command,
                       int always_run)
{
    struct tsched_step *steps;
    size_t capacity;
    if (!*command || strlen(command) >= TSCHED_COMMAND_LEN ||
        task->step_count >= TSCHED_MAX_STEPS)
        return -1;
    if (task->step_count == task->step_capacity) {
        capacity = task->step_capacity ? task->step_capacity * 2U : 4U;
        if (capacity > TSCHED_MAX_STEPS)
            capacity = TSCHED_MAX_STEPS;
        steps = realloc(task->steps, capacity * sizeof(*steps));
        if (!steps)
            return -1;
        task->steps = steps;
        task->step_capacity = capacity;
    }
    task->steps[task->step_count].command = strdup(command);
    if (!task->steps[task->step_count].command)
        return -1;
    task->steps[task->step_count].always_run = always_run;
    ++task->step_count;
    return 0;
}

static int parse_task_header(const char *header, uint32_t *id)
{
    char tail;
    return sscanf(header, "[task:%u]%c", id, &tail) == 1;
}

static int duplicate_id(const struct tsched_config *config, uint32_t id)
{
    size_t i;
    for (i = 0; i < config->task_count; ++i)
        if (config->tasks[i].id == id)
            return 1;
    return 0;
}

static int validate_task(const struct tsched_task *task, const char *path,
                         unsigned int line, char *error, size_t error_size)
{
    if (!task)
        return 0;
    if (!task->name[0]) {
        snprintf(error, error_size, "%s:%u: task %u has no name",
                 path, line, task->id);
        return -1;
    }
    if (!task->step_count) {
        snprintf(error, error_size, "%s:%u: task %u has no steps",
                 path, line, task->id);
        return -1;
    }
    if (task->schedule == TSCHED_INTERVAL && !task->interval_ms) {
        snprintf(error, error_size, "%s:%u: interval task %u has zero interval",
                 path, line, task->id);
        return -1;
    }
    return 0;
}

static int load_tasks(struct tsched_config *config, const char *path,
                      char *error, size_t error_size)
{
    if (verify_checksum(path, error, error_size) != 0)
        return -1;
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
        uint64_t wide;
        ++line_number;
        if (*key == '#' || *key == ';' || *key == '\0')
            continue;
        if (*key == '[') {
            uint32_t id;
            if (validate_task(task, path, line_number, error, error_size))
                goto fail;
            task = NULL;
            if (!parse_task_header(key, &id))
                continue;
            if (duplicate_id(config, id)) {
                snprintf(error, error_size, "%s:%u: duplicate task id %u",
                         path, line_number, id);
                goto fail;
            }
            task = append_task(config);
            if (!task) {
                snprintf(error, error_size, "%s:%u: too many tasks or out of memory",
                         path, line_number);
                goto fail;
            }
            task->id = id;
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
        if (!strcmp(key, "name")) {
            if (copy_value(task->name, sizeof(task->name), value))
                goto invalid;
        } else if (!strcmp(key, "enabled")) {
            if (parse_bool(value, &task->enabled))
                goto invalid;
        } else if (!strcmp(key, "schedule")) {
            if (!strcmp(value, "interval"))
                task->schedule = TSCHED_INTERVAL;
            else if (!strcmp(value, "manual"))
                task->schedule = TSCHED_MANUAL;
            else
                goto invalid;
        } else if (!strcmp(key, "interval_ms")) {
            if (parse_u64(value, UINT64_MAX, &wide))
                goto invalid;
            task->interval_ms = wide;
        } else if (!strcmp(key, "max_runs")) {
            if (parse_u32(value, UINT32_MAX, &task->max_runs))
                goto invalid;
        } else if (!strcmp(key, "timeout_ms")) {
            if (parse_u32(value, UINT32_MAX, &task->timeout_ms))
                goto invalid;
        } else if (!strcmp(key, "retry_count")) {
            if (parse_u32(value, 1000U, &task->retry_count))
                goto invalid;
        } else if (!strcmp(key, "workdir")) {
            if (copy_value(task->workdir, sizeof(task->workdir), value))
                goto invalid;
        } else if (!strcmp(key, "step") || !strcmp(key, "always_step")) {
            if (append_step(task, value, !strcmp(key, "always_step")))
                goto invalid;
        }
        continue;
invalid:
        config_error(error, error_size, path, line_number, key);
        goto fail;
    }
    if (ferror(file)) {
        snprintf(error, error_size, "%s: read failed", path);
        goto fail;
    }
    if (validate_task(task, path, line_number, error, error_size))
        goto fail;
    fclose(file);
    return 0;
fail:
    fclose(file);
    return -1;
}

int tsched_config_load(struct tsched_config *config, const char *global_path,
                       const char *task_path, char *error, size_t error_size)
{
    char global_backup[TSCHED_PATH_LEN + 8];
    if (error_size)
        error[0] = '\0';
    tsched_config_defaults(config);
    if (load_global(config, global_path, error, error_size) != 0) {
        tsched_config_free(config);
        tsched_config_defaults(config);
        if (snprintf(global_backup, sizeof(global_backup), "%s.bak",
                     global_path) >= (int)sizeof(global_backup) ||
            access(global_backup, R_OK) != 0 ||
            load_global(config, global_backup, error, error_size) != 0) {
            if (!error[0])
                snprintf(error, error_size, "%s: %s", global_path,
                         strerror(errno));
            return -1;
        }
    }
    if (!task_path)
        task_path = config->task_file;
    if (copy_value(config->task_file, sizeof(config->task_file), task_path)) {
        snprintf(error, error_size, "task file path is too long");
        return -1;
    }
    if (load_tasks(config, task_path, error, error_size) != 0) {
        char backup[TSCHED_PATH_LEN + 8];
        tsched_config_free(config);
        if (snprintf(backup, sizeof(backup), "%s.bak", task_path) >=
                (int)sizeof(backup) ||
            access(backup, R_OK) != 0 ||
            load_tasks(config, backup, error, error_size) != 0) {
            tsched_config_free(config);
            return -1;
        }
    }
    return 0;
}

static int sync_parent_directory(const char *path)
{
    char directory[TSCHED_PATH_LEN];
    char *slash;
    int fd;
    if (copy_value(directory, sizeof(directory), path))
        return -1;
    slash = strrchr(directory, '/');
    if (!slash)
        strcpy(directory, ".");
    else if (slash == directory)
        slash[1] = '\0';
    else
        *slash = '\0';
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

int tsched_config_save_tasks(const struct tsched_config *config,
                             const char *path, char *error, size_t error_size)
{
    char temporary[TSCHED_PATH_LEN + 8];
    char backup[TSCHED_PATH_LEN + 8];
    FILE *file = NULL;
    size_t i, j;
    int saved_errno;
    uint32_t checksum;
    if (snprintf(temporary, sizeof(temporary), "%s.new", path) >=
            (int)sizeof(temporary) ||
        snprintf(backup, sizeof(backup), "%s.bak", path) >= (int)sizeof(backup)) {
        errno = ENAMETOOLONG;
        goto fail;
    }
    file = fopen(temporary, "w+");
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
    if (ferror(file) || fflush(file) != 0 ||
        crc32_fd(fileno(file), &checksum) != 0 ||
        fseek(file, 0, SEEK_END) != 0 ||
        fprintf(file, "checksum=%08x\n", checksum) < 0 ||
        fflush(file) != 0 || fsync(fileno(file)) != 0)
        goto fail;
    if (fclose(file) != 0) {
        file = NULL;
        goto fail;
    }
    file = NULL;
    (void)unlink(backup);
    if (access(path, F_OK) == 0 && rename(path, backup) != 0)
        goto fail;
    if (rename(temporary, path) != 0) {
        saved_errno = errno;
        (void)rename(backup, path);
        errno = saved_errno;
        goto fail;
    }
    if (sync_parent_directory(path) != 0)
        goto fail;
    return 0;
fail:
    saved_errno = errno;
    if (file)
        fclose(file);
    unlink(temporary);
    snprintf(error, error_size, "%s: %s", path, strerror(saved_errno));
    errno = saved_errno;
    return -1;
}

int tsched_config_save_global(const struct tsched_config *config,
                              const char *path, char *error, size_t error_size)
{
    char temporary[TSCHED_PATH_LEN + 8];
    char backup[TSCHED_PATH_LEN + 8];
    FILE *file = NULL;
    int saved_errno;
    uint32_t checksum;
    if (snprintf(temporary, sizeof(temporary), "%s.new", path) >=
            (int)sizeof(temporary) ||
        snprintf(backup, sizeof(backup), "%s.bak", path) >=
            (int)sizeof(backup)) {
        errno = ENAMETOOLONG;
        goto fail;
    }
    file = fopen(temporary, "w+");
    if (!file)
        goto fail;
    fprintf(file,
            "[global]\n"
            "socket_path=%s\n"
            "task_file=%s\n"
            "log_dir=%s\n"
            "max_running=%u\n"
            "startup_jitter_ms=%u\n"
            "local_log_kb=%u\n\n"
            "local_log_total_kb=%u\n\n"
            "[udp_log]\n"
            "udp_enabled=%d\n"
            "udp_host=%s\n"
            "udp_port=%u\n",
            config->socket_path, config->task_file, config->log_dir,
            config->max_running, config->startup_jitter_ms,
            config->local_log_kb,
            config->local_log_total_kb,
            config->udp_enabled, config->udp_host, config->udp_port);
    if (ferror(file) || fflush(file) != 0 ||
        crc32_fd(fileno(file), &checksum) != 0 ||
        fseek(file, 0, SEEK_END) != 0 ||
        fprintf(file, "checksum=%08x\n", checksum) < 0 ||
        fflush(file) != 0 || fsync(fileno(file)) != 0)
        goto fail;
    if (fclose(file) != 0) {
        file = NULL;
        goto fail;
    }
    file = NULL;
    (void)unlink(backup);
    if (access(path, F_OK) == 0 && rename(path, backup) != 0)
        goto fail;
    if (rename(temporary, path) != 0) {
        saved_errno = errno;
        (void)rename(backup, path);
        errno = saved_errno;
        goto fail;
    }
    if (sync_parent_directory(path) != 0)
        goto fail;
    return 0;
fail:
    saved_errno = errno;
    if (file)
        fclose(file);
    unlink(temporary);
    snprintf(error, error_size, "%s: %s", path, strerror(saved_errno));
    errno = saved_errno;
    return -1;
}
