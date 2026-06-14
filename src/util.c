#define _GNU_SOURCE
#include "tsched/tsched.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t tsched_monotonic_ms(void)
{
    /* 单调时钟不受 NTP 校时或用户修改系统时间影响。 */
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

int tsched_set_nonblock_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int fd_flags;
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

int tsched_mkdir_p(const char *path)
{
    char copy[TSCHED_PATH_LEN];
    char *cursor;
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(copy, path, length + 1);
    /* 临时写入字符串终止符，逐级创建每一段父目录。 */
    for (cursor = copy + 1; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    return (mkdir(copy, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}
