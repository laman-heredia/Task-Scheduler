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
    unlink(global);
    unlink(tasks);
    return 0;
}
