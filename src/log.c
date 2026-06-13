#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int tsched_udp_open(const struct tsched_config *config,
                    struct sockaddr_storage *address, socklen_t *address_len)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port[16];
    int fd;
    /* 未启用时返回 -1，调用方将其视为“无远程日志”而不是启动错误。 */
    if (!config->udp_enabled || !config->udp_host[0] || !config->udp_port)
        return -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(port, sizeof(port), "%u", config->udp_port);
    if (getaddrinfo(config->udp_host, port, &hints, &result) != 0)
        return -1;
    fd = socket(result->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        memcpy(address, result->ai_addr, result->ai_addrlen);
        *address_len = (socklen_t)result->ai_addrlen;
    }
    freeaddrinfo(result);
    return fd;
}

void tsched_udp_send(int fd, const struct sockaddr_storage *address,
                     socklen_t address_len, const struct tsched_task *task,
                     const char *event, const char *message)
{
    /*
     * 1200 字节低于常见以太网 MTU，能够降低 UDP 报文发生 IP 分片的概率。
     * V1 采用尽力发送：不重传、不持久化，也不让日志故障阻塞调度器。
     */
    char packet[1200];
    struct timespec now;
    int length;
    if (fd < 0)
        return;
    clock_gettime(CLOCK_REALTIME, &now);
    length = snprintf(packet, sizeof(packet),
                      "TSCHED v=1 task=%u name=\"%s\" event=%s ts=%lld.%03ld msg=\"%s\"",
                      task ? task->id : 0U, task ? task->name : "daemon", event,
                      (long long)now.tv_sec, now.tv_nsec / 1000000L,
                      message ? message : "");
    if (length < 0)
        return;
    if ((size_t)length >= sizeof(packet))
        length = (int)sizeof(packet) - 1;
    (void)sendto(fd, packet, (size_t)length, MSG_DONTWAIT,
                 (const struct sockaddr *)address, address_len);
}
