#include "tsched/tsched.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_socket(const char *path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int expect_response(int fd, const char *expected)
{
    char response[256];
    size_t used = 0;
    while (used < sizeof(response) - 1U) {
        ssize_t count = read(fd, response + used, sizeof(response) - 1U - used);
        if (count > 0)
            used += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    response[used] = '\0';
    return strstr(response, expected) ? 0 : -1;
}

static int request_expect(const char *path, const char *request,
                          const char *expected)
{
    int fd = connect_socket(path);
    size_t length = strlen(request);
    if (fd < 0)
        return -1;
    if (write(fd, request, length) != (ssize_t)length ||
        shutdown(fd, SHUT_WR) != 0 ||
        expect_response(fd, expected) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    char oversized[TSCHED_IPC_REQUEST_LEN + 128U];
    int fd;
    if (argc == 3 && !strcmp(argv[2], "idle-timeout")) {
        int clients[TSCHED_MAX_CLIENTS];
        size_t index;
        for (index = 0; index < TSCHED_MAX_CLIENTS; ++index) {
            clients[index] = connect_socket(argv[1]);
            if (clients[index] < 0)
                return 1;
        }
        sleep(6);
        for (index = 0; index < TSCHED_MAX_CLIENTS; ++index)
            close(clients[index]);
        return request_expect(argv[1], "PING\n", "OK pong");
    }
    if (argc == 3 && !strcmp(argv[2], "slow-list")) {
        char response[65536];
        size_t used = 0;
        int receive_buffer = 1024;
        fd = connect_socket(argv[1]);
        if (fd < 0)
            return 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                         &receive_buffer, sizeof(receive_buffer));
        if (write(fd, "LIST\n", 5) != 5 || shutdown(fd, SHUT_WR) != 0) {
            close(fd);
            return 1;
        }
        usleep(200000);
        while (used + 1U < sizeof(response)) {
            ssize_t count = read(fd, response + used,
                                 sizeof(response) - used - 1U);
            if (count > 0)
                used += (size_t)count;
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
        response[used] = '\0';
        close(fd);
        return strstr(response, "\n256\t") ? 0 : 1;
    }
    if (argc != 2)
        return 2;

    fd = connect_socket(argv[1]);
    if (fd < 0)
        return 1;
    if (write(fd, "STA", 3) != 3) {
        close(fd);
        return 1;
    }
    usleep(20000);
    if (write(fd, "TUS\n", 4) != 4 ||
        shutdown(fd, SHUT_WR) != 0 ||
        expect_response(fd, "OK tasks=") != 0) {
        close(fd);
        return 1;
    }
    close(fd);

    if (request_expect(argv[1], "PING_GARBAGE\n", "ERR unknown command") ||
        request_expect(argv[1], "STOP_GARBAGE\n", "ERR unknown command") ||
        request_expect(argv[1], "RUN 1 trailing\n", "ERR unknown command") ||
        request_expect(argv[1], "CONFIG trailing\n", "ERR unknown command"))
        return 1;

    memset(oversized, 'X', sizeof(oversized));
    oversized[sizeof(oversized) - 1U] = '\n';
    fd = connect_socket(argv[1]);
    if (fd < 0)
        return 1;
    if (write(fd, oversized, sizeof(oversized)) != (ssize_t)sizeof(oversized) ||
        shutdown(fd, SHUT_WR) != 0 ||
        expect_response(fd, "ERR request too long") != 0) {
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}
