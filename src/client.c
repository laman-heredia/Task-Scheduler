#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void usage(const char *name)
{
    fprintf(stderr,
            "Usage: %s [-s socket] status|list|run ID|cancel ID|"
            "enable ID|disable ID|delete ID|log ID|stop\n", name);
}

static int connect_socket(const char *socket_path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int request(const char *socket_path, const char *command,
                   char *response, size_t response_size)
{
    /* CLI 和 CGI 复用同一套 Unix Socket 请求实现，减少二进制体积。 */
    size_t used = 0;
    size_t sent = 0;
    size_t command_size = strlen(command);
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return -1;
    while (sent < command_size) {
        ssize_t count = send(fd, command + sent, command_size - sent, MSG_NOSIGNAL);
        if (count > 0)
            sent += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else {
            close(fd);
            return -1;
        }
    }
    while (used + 1 < response_size) {
        ssize_t count = recv(fd, response + used, response_size - used - 1, 0);
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    response[used] = '\0';
    close(fd);
    return 0;
}

static int query_value(const char *query, const char *name,
                       char *value, size_t size)
{
    /*
     * 仅解析当前 CGI 所需的简单 key=value 参数，不引入完整 URL/JSON 库。
     * 调用方仍通过 strtoul 限定任务 ID 为数字。
     */
    size_t name_length = strlen(name);
    while (query && *query) {
        const char *end = strchr(query, '&');
        size_t field_length = end ? (size_t)(end - query) : strlen(query);
        if (field_length > name_length && !strncmp(query, name, name_length) &&
            query[name_length] == '=') {
            const char *source = query + name_length + 1;
            const char *limit = query + field_length;
            size_t used = 0;
            while (source < limit) {
                unsigned char decoded;
                if (used + 1 >= size)
                    return -1;
                if (*source == '+') {
                    decoded = ' ';
                    ++source;
                } else if (*source == '%' && source + 2 < limit &&
                           isxdigit((unsigned char)source[1]) &&
                           isxdigit((unsigned char)source[2])) {
                    char hex[3] = {source[1], source[2], '\0'};
                    decoded = (unsigned char)strtoul(hex, NULL, 16);
                    source += 3;
                } else if (*source == '%') {
                    return -1;
                } else {
                    decoded = (unsigned char)*source++;
                }
                if (!decoded)
                    return -1;
                value[used++] = (char)decoded;
            }
            value[used] = '\0';
            return 1;
        }
        query = end ? end + 1 : NULL;
    }
    return 0;
}

static void json_escape(const char *source, char *destination, size_t size)
{
    /* 输出 JSON 前转义引号和反斜杠，并过滤 ASCII 控制字符。 */
    size_t used = 0;
    while (*source && used + 2 < size) {
        unsigned char character = (unsigned char)*source++;
        if (character == '"' || character == '\\') {
            destination[used++] = '\\';
            destination[used++] = (char)character;
        } else if (character >= 0x20) {
            destination[used++] = (char)character;
        }
    }
    destination[used] = '\0';
}

static int cgi_main(void)
{
    /* uHTTPd 设置 GATEWAY_INTERFACE 后，同一可执行文件切换为 CGI 模式。 */
    const char *query = getenv("QUERY_STRING");
    const char *socket_path = getenv("TSCHED_SOCKET");
    char action[32] = "";
    char id[16] = "";
    char enabled[8] = "";
    char host[64] = "";
    char port[8] = "";
    char name[TSCHED_NAME_LEN] = "";
    char schedule[16] = "";
    char interval[24] = "";
    char max_runs[16] = "";
    char timeout[16] = "";
    char retry[16] = "";
    char workdir[TSCHED_PATH_LEN] = "";
    char task_command[TSCHED_COMMAND_LEN] = "";
    char command[1600];
    char response[TSCHED_IPC_RESPONSE_LEN];
    int invalid_query = 0;
    if (!socket_path)
        socket_path = "/tmp/tsched.sock";
    invalid_query |= query_value(query, "action", action, sizeof(action)) < 0;
    invalid_query |= query_value(query, "id", id, sizeof(id)) < 0;
    invalid_query |= query_value(query, "enabled", enabled, sizeof(enabled)) < 0;
    invalid_query |= query_value(query, "host", host, sizeof(host)) < 0;
    invalid_query |= query_value(query, "port", port, sizeof(port)) < 0;
    invalid_query |= query_value(query, "name", name, sizeof(name)) < 0;
    invalid_query |= query_value(query, "schedule", schedule,
                                 sizeof(schedule)) < 0;
    invalid_query |= query_value(query, "interval", interval,
                                 sizeof(interval)) < 0;
    invalid_query |= query_value(query, "max_runs", max_runs,
                                 sizeof(max_runs)) < 0;
    invalid_query |= query_value(query, "timeout", timeout,
                                 sizeof(timeout)) < 0;
    invalid_query |= query_value(query, "retry", retry, sizeof(retry)) < 0;
    invalid_query |= query_value(query, "workdir", workdir,
                                 sizeof(workdir)) < 0;
    invalid_query |= query_value(query, "command", task_command,
                                 sizeof(task_command)) < 0;
    if (invalid_query) {
        printf("Status: 400 Bad Request\r\n"
               "Content-Type: application/json\r\n\r\n"
               "{\"error\":\"invalid or oversized query parameter\"}\n");
        return 0;
    }
    if (!strcmp(action, "tasks"))
        snprintf(command, sizeof(command), "LIST\n");
    else if (!strcmp(action, "status"))
        snprintf(command, sizeof(command), "STATUS\n");
    else if (!strcmp(action, "run") && id[0])
        snprintf(command, sizeof(command), "RUN %lu\n", strtoul(id, NULL, 10));
    else if (!strcmp(action, "cancel") && id[0])
        snprintf(command, sizeof(command), "CANCEL %lu\n", strtoul(id, NULL, 10));
    else if (!strcmp(action, "config"))
        snprintf(command, sizeof(command), "CONFIG\n");
    else if (!strcmp(action, "config-save") && enabled[0] && host[0] && port[0])
        snprintf(command, sizeof(command), "SETUDP %lu %s %lu\n",
                 strtoul(enabled, NULL, 10), host, strtoul(port, NULL, 10));
    else if (!strcmp(action, "task-save") && id[0] && name[0] &&
             enabled[0] && schedule[0] && interval[0] && max_runs[0] &&
             timeout[0] && retry[0] && task_command[0])
        snprintf(command, sizeof(command),
                 "UPSERT\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                 id, name, enabled, schedule, interval, max_runs, timeout,
                 retry, workdir[0] ? workdir : "/", task_command);
    else if (!strcmp(action, "task-delete") && id[0])
        snprintf(command, sizeof(command), "DELETE %lu\n", strtoul(id, NULL, 10));
    else if (!strcmp(action, "task") && id[0])
        snprintf(command, sizeof(command), "GET %lu\n", strtoul(id, NULL, 10));
    else if (!strcmp(action, "task-enable") && id[0] && enabled[0])
        snprintf(command, sizeof(command), "ENABLE %lu %lu\n",
                 strtoul(id, NULL, 10), strtoul(enabled, NULL, 10));
    else if (!strcmp(action, "log") && id[0])
        snprintf(command, sizeof(command), "LOG %lu\n", strtoul(id, NULL, 10));
    else {
        printf("Status: 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
               "{\"error\":\"unsupported action\"}\n");
        return 0;
    }
    if (request(socket_path, command, response, sizeof(response)) != 0) {
        printf("Status: 503 Service Unavailable\r\nContent-Type: application/json\r\n\r\n"
               "{\"error\":\"daemon unavailable\"}\n");
        return 0;
    }
    printf("Content-Type: application/json\r\nCache-Control: no-store\r\n\r\n");
    if (!strcmp(action, "tasks")) {
        char *line = strchr(response, '\n');
        int first = 1;
        unsigned int task_id, task_enabled, state, runs, run_id;
        size_t active_step, step_count;
        int exit_code;
        char task_name[TSCHED_NAME_LEN];
        printf("{\"tasks\":[");
        while (line && *++line) {
            char *next = strchr(line, '\n');
            if (next)
                *next = '\0';
            if (sscanf(line, "%u\t%63[^\t]\t%u\t%u\t%u\t%u\t%d\t%zu\t%zu",
                       &task_id, task_name, &task_enabled, &state, &runs,
                       &run_id, &exit_code, &active_step, &step_count) == 9) {
                char escaped[TSCHED_NAME_LEN * 2];
                json_escape(task_name, escaped, sizeof(escaped));
                printf("%s{\"id\":%u,\"name\":\"%s\",\"enabled\":%u,"
                       "\"state\":%u,\"runs\":%u,\"run_id\":%u,"
                       "\"exit_code\":%d,\"active_step\":%zu,"
                       "\"step_count\":%zu}",
                       first ? "" : ",", task_id, escaped, task_enabled, state,
                       runs, run_id, exit_code, active_step, step_count);
                first = 0;
            }
            line = next;
        }
        printf("]}\n");
    } else if (!strcmp(action, "config")) {
        unsigned int udp_enabled, udp_port;
        char udp_host[64], escaped[128];
        if (sscanf(response, "OK %u\t%63[^\t]\t%u",
                   &udp_enabled, udp_host, &udp_port) != 3) {
            printf("{\"error\":\"invalid daemon response\"}\n");
        } else {
            json_escape(udp_host, escaped, sizeof(escaped));
            printf("{\"udp_enabled\":%u,\"udp_host\":\"%s\",\"udp_port\":%u}\n",
                   udp_enabled, !strcmp(escaped, "-") ? "" : escaped, udp_port);
        }
    } else if (!strcmp(action, "task")) {
        unsigned int task_id, task_enabled, task_max_runs, task_timeout, task_retry;
        unsigned long long task_interval;
        size_t task_step_count;
        char task_name[TSCHED_NAME_LEN], task_schedule[16];
        char task_workdir[TSCHED_PATH_LEN], shell_command[TSCHED_COMMAND_LEN];
        char escaped_name[TSCHED_NAME_LEN * 2], escaped_dir[TSCHED_PATH_LEN * 2];
        char escaped_command[TSCHED_COMMAND_LEN * 2];
        if (sscanf(response, "OK\t%u\t%63[^\t]\t%u\t%15[^\t]\t%llu\t"
                   "%u\t%u\t%u\t%255[^\t]\t%zu\t%1023[^\n]",
                   &task_id, task_name, &task_enabled, task_schedule,
                   &task_interval, &task_max_runs, &task_timeout, &task_retry,
                   task_workdir, &task_step_count, shell_command) != 11) {
            printf("{\"error\":\"invalid daemon response\"}\n");
        } else {
            json_escape(task_name, escaped_name, sizeof(escaped_name));
            json_escape(task_workdir, escaped_dir, sizeof(escaped_dir));
            json_escape(shell_command, escaped_command, sizeof(escaped_command));
            printf("{\"id\":%u,\"name\":\"%s\",\"enabled\":%u,"
                   "\"schedule\":\"%s\",\"interval\":%llu,\"max_runs\":%u,"
                   "\"timeout\":%u,\"retry\":%u,\"workdir\":\"%s\","
                   "\"step_count\":%zu,\"command\":\"%s\"}\n",
                   task_id, escaped_name, task_enabled, task_schedule,
                   task_interval, task_max_runs, task_timeout, task_retry,
                   escaped_dir, task_step_count, escaped_command);
        }
    } else if (!strcmp(action, "log")) {
        char escaped[16000];
        const char *body = !strncmp(response, "OK\n", 3) ? response + 3 : response;
        json_escape(body, escaped, sizeof(escaped));
        printf("{\"log\":\"%s\"}\n", escaped);
    } else {
        char escaped[8192];
        json_escape(response, escaped, sizeof(escaped));
        printf("{\"result\":\"%s\"}\n", escaped);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *socket_path = "/tmp/tsched.sock";
    const char *action;
    char command[128];
    char response[TSCHED_IPC_RESPONSE_LEN];
    int index = 1;
    if (getenv("GATEWAY_INTERFACE"))
        return cgi_main();
    if (argc > 3 && !strcmp(argv[1], "-s")) {
        socket_path = argv[2];
        index = 3;
    }
    if (index >= argc) {
        usage(argv[0]);
        return 2;
    }
    action = argv[index++];
    if (!strcmp(action, "status"))
        snprintf(command, sizeof(command), "STATUS\n");
    else if (!strcmp(action, "list"))
        snprintf(command, sizeof(command), "LIST\n");
    else if (!strcmp(action, "stop"))
        snprintf(command, sizeof(command), "STOP\n");
    else if ((!strcmp(action, "run") || !strcmp(action, "cancel") ||
              !strcmp(action, "delete") || !strcmp(action, "log")) &&
             index < argc)
        snprintf(command, sizeof(command), "%s %lu\n",
                 !strcmp(action, "run") ? "RUN" :
                 !strcmp(action, "cancel") ? "CANCEL" :
                 !strcmp(action, "delete") ? "DELETE" : "LOG",
                 strtoul(argv[index], NULL, 10));
    else if ((!strcmp(action, "enable") || !strcmp(action, "disable")) &&
             index < argc)
        snprintf(command, sizeof(command), "ENABLE %lu %d\n",
                 strtoul(argv[index], NULL, 10),
                 !strcmp(action, "enable") ? 1 : 0);
    else {
        usage(argv[0]);
        return 2;
    }
    if (request(socket_path, command, response, sizeof(response)) != 0) {
        fprintf(stderr, "%s: %s\n", socket_path, strerror(errno));
        return 1;
    }
    fputs(response, stdout);
    return 0;
}
