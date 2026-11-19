// client.c: общая логика сетевых клиентов (автомобили, пешеходы, наблюдатели)

#include "client.h"
#include "app.h"
#include "io.h"
#include "net.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void client_init(ClientConnection *cli, const char *ip, const char *port,
                 ClientRole role, int direction, const char *name)
{
    memset(cli, 0, sizeof(*cli));
    cli->fd = -1;
    cli->role = role;
    cli->direction = direction;
    cli->next_seq = 1;
    cli->auto_reconnect = 1;
    cli->retry_interval_ms = 500;
    cli->max_retries = 5;

    snprintf(cli->ip, sizeof(cli->ip), "%s", ip ? ip : "127.0.0.1");
    snprintf(cli->port, sizeof(cli->port), "%s", port ? port : "45678");
    if (name && *name)
        snprintf(cli->name, sizeof(cli->name), "%s", name);
}

int client_is_connected(const ClientConnection *cli)
{
    return cli && cli->fd >= 0;
}

void client_disconnect(ClientConnection *cli)
{
    if (!cli) return;
    if (cli->fd >= 0) {
        close(cli->fd);
        cli->fd = -1;
    }
}

static uint32_t client_next_seq(ClientConnection *cli)
{
    return cli->next_seq++;
}

// Одиночная попытка подключения и handshake
static int client_connect_single(ClientConnection *cli)
{
    client_disconnect(cli);

    int fd = net_connect(cli->ip, cli->port);
    if (fd < 0) return -1;

    cli->fd = fd;

    // Формируем и отправляем HELLO
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.hello.role = (uint8_t)cli->role;
    p.hello.direction = (uint8_t)(cli->direction >= 0 ? cli->direction : 0);
    snprintf(p.hello.name, sizeof(p.hello.name), "%s", cli->name);

    ProtoMessage m;
    uint32_t seq = client_next_seq(cli);
    if (proto_build(&m, MSG_HELLO, seq, &p) != 0 || proto_send(cli->fd, &m) != 0) {
        client_disconnect(cli);
        return -1;
    }

    // Ожидаем HELLO_ACK (таймаут 5000 мс)
    struct pollfd pfd;
    pfd.fd = cli->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, 5000);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        client_disconnect(cli);
        return -1;
    }

    ProtoMessage ack_msg;
    int rc = proto_recv(cli->fd, &ack_msg);
    if (rc <= 0) {
        client_disconnect(cli);
        return -1;
    }

    if (ack_msg.type != MSG_HELLO_ACK) {
        if (ack_msg.type == MSG_ERROR) {
            ProtoPayload err_p;
            if (proto_decode(&ack_msg, &err_p) == 0) {
                io_printf(2, "[клиент] сервер отклонил HELLO: %s (код %d)\n",
                          err_p.error.text, err_p.error.code);
            }
        }
        client_disconnect(cli);
        return -1;
    }

    ProtoPayload ack_payload;
    if (proto_decode(&ack_msg, &ack_payload) != 0) {
        client_disconnect(cli);
        return -1;
    }

    cli->client_id = ack_payload.hello_ack.client_id;
    cli->cfg = ack_payload.hello_ack;

    return 0;
}

int client_connect_with_retry(ClientConnection *cli, int max_retries,
                              int retry_interval_ms)
{
    if (!cli) return -1;
    int attempt = 0;

    while (max_retries < 0 || attempt < max_retries) {
        if (app_signal_received())
            return -1;

        if (client_connect_single(cli) == 0)
            return 0;

        attempt++;
        if (max_retries >= 0 && attempt >= max_retries)
            break;

        // Спим небольшими квантами, чтобы мгновенно реагировать на сигналы
        int left_ms = retry_interval_ms > 0 ? retry_interval_ms : 500;
        while (left_ms > 0 && !app_signal_received()) {
            int step = left_ms > 50 ? 50 : left_ms;
            usleep((useconds_t)step * 1000);
            left_ms -= step;
        }
    }

    return -1;
}

int client_reconnect_if_needed(ClientConnection *cli)
{
    if (!cli) return -1;
    if (client_is_connected(cli))
        client_disconnect(cli);

    return client_connect_with_retry(cli, cli->max_retries, cli->retry_interval_ms);
}

int client_recv_message_timeout(ClientConnection *cli, ProtoMessage *m,
                                ProtoPayload *p, int timeout_ms)
{
    if (!cli || cli->fd < 0) return -1;

    struct pollfd pfd;
    pfd.fd = cli->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;
        client_disconnect(cli);
        return -1;
    }
    if (pr == 0) return 0; // таймаут

    if (!(pfd.revents & POLLIN)) {
        client_disconnect(cli);
        return -1;
    }

    int rc = proto_recv(cli->fd, m);
    if (rc <= 0) {
        client_disconnect(cli);
        return -1; // EOF или ошибка
    }

    if (p && proto_decode(m, p) != 0) {
        return -1;
    }

    if (m->type == MSG_SHUTDOWN) {
        return 2; // сервер завершает работу
    }

    return 1;
}

int client_send_car(ClientConnection *cli, int maneuver, int emergency,
                    int *out_accepted)
{
    if (!cli) return -1;
    if (out_accepted) *out_accepted = 0;

    if (cli->fd < 0) {
        if (!cli->auto_reconnect || client_reconnect_if_needed(cli) != 0)
            return -1;
    }

    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.car_arrival.client_id = cli->client_id;
    p.car_arrival.direction = (uint8_t)cli->direction;
    p.car_arrival.maneuver = (uint8_t)maneuver;
    p.car_arrival.emergency = (uint8_t)(emergency ? 1 : 0);

    ProtoMessage req;
    uint32_t seq = client_next_seq(cli);
    if (proto_build(&req, MSG_CAR_ARRIVAL, seq, &p) != 0 ||
        proto_send(cli->fd, &req) != 0) {
        client_disconnect(cli);
        return -1;
    }

    // Ожидаем ответ от сервера (ACK или SHUTDOWN)
    ProtoMessage resp;
    ProtoPayload resp_p;
    while (!app_signal_received()) {
        int r = client_recv_message_timeout(cli, &resp, &resp_p, 3000);
        if (r == 0) {
            // Таймаут ответа
            client_disconnect(cli);
            return -1;
        }
        if (r == 2) {
            // MSG_SHUTDOWN
            return 1;
        }
        if (r < 0) {
            return -1;
        }

        if (resp.type == MSG_ACK) {
            if (resp_p.ack.ref_seq == seq) {
                if (out_accepted)
                    *out_accepted = (resp_p.ack.code == 0 ? 1 : 0);
                return 0;
            }
        } else if (resp.type == MSG_ERROR) {
            io_printf(2, "[клиент-авто] ошибка сервера: %s (код %d)\n",
                      resp_p.error.text, resp_p.error.code);
            if (out_accepted) *out_accepted = 0;
            return 0;
        }
    }

    return -1;
}

int client_send_ped(ClientConnection *cli, int *out_accepted)
{
    if (!cli) return -1;
    if (out_accepted) *out_accepted = 0;

    if (cli->fd < 0) {
        if (!cli->auto_reconnect || client_reconnect_if_needed(cli) != 0)
            return -1;
    }

    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.ped_request.client_id = cli->client_id;
    p.ped_request.direction = (uint8_t)cli->direction;

    ProtoMessage req;
    uint32_t seq = client_next_seq(cli);
    if (proto_build(&req, MSG_PED_REQUEST, seq, &p) != 0 ||
        proto_send(cli->fd, &req) != 0) {
        client_disconnect(cli);
        return -1;
    }

    // Ожидаем ответ от сервера (ACK или SHUTDOWN)
    ProtoMessage resp;
    ProtoPayload resp_p;
    while (!app_signal_received()) {
        int r = client_recv_message_timeout(cli, &resp, &resp_p, 3000);
        if (r == 0) {
            client_disconnect(cli);
            return -1;
        }
        if (r == 2) {
            return 1;
        }
        if (r < 0) {
            return -1;
        }

        if (resp.type == MSG_ACK) {
            if (resp_p.ack.ref_seq == seq) {
                if (out_accepted)
                    *out_accepted = (resp_p.ack.code == 0 ? 1 : 0);
                return 0;
            }
        } else if (resp.type == MSG_ERROR) {
            io_printf(2, "[клиент-пешеход] ошибка сервера: %s (код %d)\n",
                      resp_p.error.text, resp_p.error.code);
            if (out_accepted) *out_accepted = 0;
            return 0;
        }
    }

    return -1;
}

int client_choose_maneuver(const ClientConnection *cli)
{
    if (!cli) return MANEUVER_STRAIGHT;

    int dir = cli->direction;
    if (dir < 0 || dir >= MAX_DIRECTIONS)
        return MANEUVER_STRAIGHT;

    int allowed[MANEUVER_COUNT];
    int count = 0;

    for (int m = 0; m < MANEUVER_COUNT; ++m) {
        if (cli->cfg.maneuvers[dir][m]) {
            allowed[count++] = m;
        }
    }

    if (count == 0)
        return MANEUVER_STRAIGHT;

    return allowed[rand() % count];
}

int client_is_maneuver_allowed(const ClientConnection *cli, int maneuver)
{
    if (!cli) return 0;
    int dir = cli->direction;
    if (dir < 0 || dir >= MAX_DIRECTIONS) return 0;
    if (maneuver < 0 || maneuver >= MANEUVER_COUNT) return 0;
    return cli->cfg.maneuvers[dir][maneuver] ? 1 : 0;
}
