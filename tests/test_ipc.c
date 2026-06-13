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

int main(int argc, char **argv)
{
    char oversized[2200];
    int fd;
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
