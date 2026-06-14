#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed: %s\n", #expression); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char global[] = "/tmp/tsched-global-XXXXXX";
    char tasks[] = "/tmp/tsched-tasks-XXXXXX";
    char error[256];
    struct tsched_config config;
    FILE *file;
    int fd = mkstemp(global);
    CHECK(fd >= 0);
    file = fdopen(fd, "w");
    CHECK(file);
    fputs("socket_path=/tmp/test.sock\nudp_enabled=1\nudp_host=127.0.0.1\n"
          "udp_port=5514\n", file);
    fclose(file);
    fd = mkstemp(tasks);
    CHECK(fd >= 0);
    file = fdopen(fd, "w");
    CHECK(file);
    fputs("[task:7]\nname=hello\nenabled=1\nschedule=interval\n"
          "interval_ms=1000\nstep=echo hello\nalways_step=echo done\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) == 0);
    CHECK(config.task_count == 1);
    CHECK(config.tasks[0].id == 7);
    CHECK(config.tasks[0].step_count == 2);
    CHECK(config.tasks[0].steps[1].always_run == 1);
    CHECK(!strcmp(config.socket_path, "/tmp/test.sock"));
    CHECK(config.local_log_total_kb == 2048);
    CHECK(config.kill_grace_ms == 3000);
    CHECK(config.retry_delay_ms == 1000);
    CHECK(config.socket_mode == 0660);
    CHECK(config.mirror_output == 0);
    CHECK(tsched_config_save_tasks(&config, tasks, error, sizeof(error)) == 0);
    config.local_log_total_kb = 1024;
    config.kill_grace_ms = 125;
    config.retry_delay_ms = 25;
    config.socket_mode = 0600;
    config.mirror_output = 1;
    CHECK(tsched_config_save_global(&config, global, error, sizeof(error)) == 0);
    config.local_log_total_kb = 512;
    CHECK(tsched_config_save_global(&config, global, error, sizeof(error)) == 0);
    CHECK(access(tasks, F_OK) == 0);
    tsched_config_free(&config);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) == 0);
    CHECK(!strcmp(config.tasks[0].name, "hello"));
    CHECK(config.local_log_total_kb == 512);
    CHECK(config.kill_grace_ms == 125);
    CHECK(config.retry_delay_ms == 25);
    CHECK(config.socket_mode == 0600);
    CHECK(config.mirror_output == 1);
    tsched_config_free(&config);
    file = fopen(global, "a");
    CHECK(file);
    fputs("tampered=1\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) == 0);
    CHECK(config.local_log_total_kb == 1024);
    tsched_config_free(&config);
    file = fopen(tasks, "a");
    CHECK(file);
    fputs("tampered=1\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) == 0);
    CHECK(!strcmp(config.tasks[0].name, "hello"));
    tsched_config_free(&config);
    file = fopen(tasks, "w");
    CHECK(file);
    fputs("[task:7]\nname=bad\nenabled=maybe\nschedule=interval\n"
          "interval_ms=0\nstep=echo bad\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) == 0);
    CHECK(!strcmp(config.tasks[0].name, "hello"));
    tsched_config_free(&config);
    {
        char backup[sizeof(tasks) + 4];
        snprintf(backup, sizeof(backup), "%s.bak", tasks);
        unlink(backup);
    }
    file = fopen(tasks, "w");
    CHECK(file);
    fputs("format_version=2\n\n[task:7]\nname=future\nenabled=1\n"
          "schedule=manual\nstep=true\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) != 0);
    CHECK(strstr(error, "unsupported format_version") != NULL);
    file = fopen(tasks, "w");
    CHECK(file);
    fputs("[task:7]\nname=misplaced\nenabled=1\nschedule=manual\n"
          "step=true\nformat_version=1\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) != 0);
    CHECK(strstr(error, "format_version must precede") != NULL);
    file = fopen(tasks, "w");
    CHECK(file);
    fputs("[task:7]\nname=bad\nenabled=maybe\nschedule=interval\n"
          "interval_ms=0\nstep=echo bad\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) != 0);
    CHECK(strstr(error, "invalid value for enabled") != NULL);
    file = fopen(tasks, "w");
    CHECK(file);
    fputs("[task:7]\nname=too-many-steps\nenabled=1\nschedule=manual\n",
          file);
    {
        int step;
        for (step = 0; step < TSCHED_MAX_STEPS + 1; ++step)
            fprintf(file, "step=echo %d\n", step);
    }
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) != 0);
    CHECK(strstr(error, "invalid value for step") != NULL);
    file = fopen(tasks, "w");
    CHECK(file);
    fputs("[task:4294967295]\nname=integer-boundaries\nenabled=1\n"
          "schedule=interval\ninterval_ms=18446744073709551615\n"
          "max_runs=4294967295\ntimeout_ms=4294967295\n"
          "retry_count=1000\nstep=true\n", file);
    fclose(file);
    CHECK(tsched_config_load(&config, global, tasks, error, sizeof(error)) == 0);
    CHECK(config.tasks[0].id == UINT32_MAX);
    CHECK(config.tasks[0].interval_ms == UINT64_MAX);
    CHECK(config.tasks[0].max_runs == UINT32_MAX);
    tsched_config_free(&config);
    unlink(global);
    unlink(tasks);
    {
        char backup[sizeof(global) + 4];
        snprintf(backup, sizeof(backup), "%s.bak", global);
        unlink(backup);
    }
    return 0;
}
