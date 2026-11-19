#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

// Один вызов io_printf формирует одну строку и делает один write()
// поэтому строки разных процессов не перемешиваются внутри себя
void io_printf(int fd, const char *fmt, ...)
{
    if (fd < 0 || !fmt) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (w == 0) return;
        done += (size_t)w;
    }
}

int io_open_read(const char *path)
{
    return open(path, O_RDONLY);
}

ssize_t io_read_line(int fd, char *buf, size_t cap)
{
    if (!buf || cap == 0) return -1;

    size_t len = 0;
    int got_any = 0;
    while (len + 1 < cap) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        got_any = 1;
        if (c == '\n') break;
        buf[len++] = c;
    }
    buf[len] = '\0';
    return got_any ? (ssize_t)len : -1;
}

int io_open_log(const char *path)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

void io_close(int fd)
{
    if (fd >= 0) close(fd);
}
