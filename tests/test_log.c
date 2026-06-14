#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed: %s\n", #expression); \
        return 1; \
    } \
} while (0)

int main(void)
{
    struct sockaddr_in local;
    struct sockaddr_storage remote;
    struct tsched_config config;
    struct tsched_task task;
    struct timeval timeout = {1, 0};
    socklen_t local_length = sizeof(local);
    socklen_t remote_length = 0;
    char packet[1400];
    int receiver;
    int sender;
    ssize_t count;

    receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(receiver >= 0);
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(receiver, (struct sockaddr *)&local, sizeof(local)) == 0);
    CHECK(getsockname(receiver, (struct sockaddr *)&local, &local_length) == 0);
    CHECK(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout)) == 0);

    tsched_config_defaults(&config);
    config.udp_enabled = 1;
    snprintf(config.udp_host, sizeof(config.udp_host), "127.0.0.1");
    config.udp_port = ntohs(local.sin_port);
    sender = tsched_udp_open(&config, &remote, &remote_length);
    CHECK(sender >= 0);

    memset(&task, 0, sizeof(task));
    task.id = 7;
    task.last_run_id = 9;
    task.active_step = 1;
    task.state = TSCHED_RUNNING;
    snprintf(task.name, sizeof(task.name), "quote\" slash\\ line\n");
    tsched_udp_send(sender, &remote, remote_length, &task,
                    "output", "a\nb\t\"c\"\\");
    count = recv(receiver, packet, sizeof(packet) - 1U, 0);
    CHECK(count > 0);
    packet[count] = '\0';
    CHECK(strstr(packet, "task=7 run=9 step=2") != NULL);
    CHECK(strstr(packet, "name=\"quote\\\" slash\\\\ line\\n\"") != NULL);
    CHECK(strstr(packet, "msg=\"a\\nb\\t\\\"c\\\"\\\\\"") != NULL);
    CHECK(strchr(packet, '\n') == NULL);

    close(sender);
    snprintf(config.udp_host, sizeof(config.udp_host), "invalid.invalid");
    CHECK(tsched_udp_open(&config, &remote, &remote_length) < 0);
    close(receiver);
    tsched_config_free(&config);
    return 0;
}
