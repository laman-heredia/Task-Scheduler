#define _GNU_SOURCE
#include "tsched/tsched.h"

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
            "Usage: %s [-s socket] status|list|run ID|cancel ID|stop\n", name);
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
    int fd = connect_socket(socket_path);
    if (fd < 0)
        return -1;
    if (send(fd, command, strlen(command), MSG_NOSIGNAL) < 0) {
        close(fd);
        return -1;
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

static const char *query_value(const char *query, const char *name,
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
            size_t value_length = field_length - name_length - 1;
            if (value_length >= size)
                value_length = size - 1;
            memcpy(value, query + name_length + 1, value_length);
            value[value_length] = '\0';
            return value;
        }
        query = end ? end + 1 : NULL;
    }
    return NULL;
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
    char command[64];
    char response[4096];
    if (!socket_path)
        socket_path = "/tmp/tsched.sock";
    query_value(query, "action", action, sizeof(action));
    query_value(query, "id", id, sizeof(id));
    if (!strcmp(action, "tasks"))
        snprintf(command, sizeof(command), "LIST\n");
    else if (!strcmp(action, "status"))
        snprintf(command, sizeof(command), "STATUS\n");
    else if (!strcmp(action, "run") && id[0])
        snprintf(command, sizeof(command), "RUN %lu\n", strtoul(id, NULL, 10));
    else if (!strcmp(action, "cancel") && id[0])
        snprintf(command, sizeof(command), "CANCEL %lu\n", strtoul(id, NULL, 10));
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
        unsigned int task_id, enabled, state, runs;
        char name[TSCHED_NAME_LEN];
        printf("{\"tasks\":[");
        while (line && *++line) {
            char *next = strchr(line, '\n');
            if (next)
                *next = '\0';
            if (sscanf(line, "%u\t%63[^\t]\t%u\t%u\t%u",
                       &task_id, name, &enabled, &state, &runs) == 5) {
                char escaped[TSCHED_NAME_LEN * 2];
                json_escape(name, escaped, sizeof(escaped));
                printf("%s{\"id\":%u,\"name\":\"%s\",\"enabled\":%u,"
                       "\"state\":%u,\"runs\":%u}",
                       first ? "" : ",", task_id, escaped, enabled, state, runs);
                first = 0;
            }
            line = next;
        }
        printf("]}\n");
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
    char response[4096];
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
    else if ((!strcmp(action, "run") || !strcmp(action, "cancel")) && index < argc)
        snprintf(command, sizeof(command), "%s %lu\n",
                 !strcmp(action, "run") ? "RUN" : "CANCEL",
                 strtoul(argv[index], NULL, 10));
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
