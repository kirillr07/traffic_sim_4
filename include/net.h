#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <sys/types.h>

// Создать слушающий TCP-сокет.
// ip == NULL или "" — слушать на всех интерфейсах.
// Возвращает дескриптор или -1.
int net_listen(const char *ip, const char *port, int backlog);

// Подключиться к TCP-серверу. Возвращает дескриптор или -1.
int net_connect(const char *ip, const char *port);

// Отправить/принять ровно len байт.
// net_recv_all: 0 — корректный EOF, -1 — ошибка, иначе число прочитанных байт.
int     net_send_all(int fd, const void *buf, size_t len);
ssize_t net_recv_all(int fd, void *buf, size_t len);

// Перевести сокет в неблокирующий режим
int net_set_nonblocking(int fd);

// Строка "ip:port" удалённой стороны (для логов)
int net_peer_name(int fd, char *buf, size_t cap);

void net_close(int fd);

#endif /* NET_H */
