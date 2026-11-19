#ifndef IO_H
#define IO_H

#include <stddef.h>
#include <sys/types.h>

// printf для файлового дескриптора
void io_printf(int fd, const char *fmt, ...);

// Ввод
int  io_open_read(const char *path);
ssize_t io_read_line(int fd, char *buf, size_t cap);

// Лог
int  io_open_log(const char *path);
void io_close(int fd);

#endif
