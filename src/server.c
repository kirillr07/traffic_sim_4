// server.c: TCP-сервер регулируемого перекрёстка.
// Принимает клиентов, ведёт модель в потоках контроллера и направлений,
// рассылает события и снимки состояния наблюдателям.

#include "app.h"
#include "events.h"
#include "io.h"
#include "model.h"
#include "net.h"
#include "protocol.h"
#include "server.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SERVER_MAX_CLIENTS   64
#define SERVER_BACKLOG       16
#define SEND_TIMEOUT_SEC     2
#define HANDSHAKE_TIMEOUT_SEC 10

// Значения событий модели и протокола должны совпадать.
#define ASSERT_EVENT(a, b) \
    _Static_assert((int)(a) == (int)(b), #a " != " #b)

ASSERT_EVENT(MODEL_EVENT_CAR_ARRIVED, EVENT_CAR_ARRIVED);
ASSERT_EVENT(MODEL_EVENT_CAR_QUEUED, EVENT_CAR_QUEUED);
ASSERT_EVENT(MODEL_EVENT_CAR_REJECTED, EVENT_CAR_REJECTED);
ASSERT_EVENT(MODEL_EVENT_CAR_PASSED, EVENT_CAR_PASSED);
ASSERT_EVENT(MODEL_EVENT_PED_REQUEST, EVENT_PED_REQUEST);
ASSERT_EVENT(MODEL_EVENT_PED_QUEUED, EVENT_PED_QUEUED);
ASSERT_EVENT(MODEL_EVENT_PED_REJECTED, EVENT_PED_REJECTED);
ASSERT_EVENT(MODEL_EVENT_PED_PASSED, EVENT_PED_PASSED);
ASSERT_EVENT(MODEL_EVENT_PHASE_START, EVENT_PHASE_START);
ASSERT_EVENT(MODEL_EVENT_PHASE_END, EVENT_PHASE_END);
ASSERT_EVENT(MODEL_EVENT_TRANSITION_START, EVENT_TRANSITION_START);
ASSERT_EVENT(MODEL_EVENT_TRANSITION_END, EVENT_TRANSITION_END);
ASSERT_EVENT(MODEL_EVENT_EMERGENCY_PRIORITY, EVENT_EMERGENCY_PRIORITY);
ASSERT_EVENT(MODEL_EVENT_SERVER_START, EVENT_SERVER_START);
ASSERT_EVENT(MODEL_EVENT_SERVER_STOP, EVENT_SERVER_STOP);

typedef struct Server Server;

typedef struct {
    Server *srv;
    int slot;
} ClientArg;

typedef struct {
    Server *srv;
    int dir;
} DirectionArg;

typedef struct {
    int fd;
    int role;
    int direction;
    uint32_t id;
    int active;
    int in_use;
    int thread_started;
    pthread_t thread;
    pthread_mutex_t send_lock;
    char peer[64];
    char name[PROTO_NAME_LEN];
} ClientSlot;

struct Server {
    SharedState st;
    int listen_fd;

    pthread_mutex_t clients_mutex;
    ClientSlot clients[SERVER_MAX_CLIENTS];
    uint32_t next_client_id;
    uint32_t next_seq;

    atomic_int stopping;

    pthread_t controller_thread;
    int controller_started;

    pthread_t direction_threads[MAX_DIRECTIONS];
    int direction_started[MAX_DIRECTIONS];
    DirectionArg dir_args[MAX_DIRECTIONS];
};

// Вспомогательные функции
static void set_socket_timeout(int fd, int option, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv));
}

static uint32_t server_next_seq(Server *srv)
{
    return __atomic_add_fetch(&srv->next_seq, 1, __ATOMIC_RELAXED);
}

// Отправка клиенту под его блокировкой: сообщения из разных потоков
// (снимки, события, ACK) не должны перемешиваться в одном сокете.
static int client_send(ClientSlot *c, const ProtoMessage *m)
{
    pthread_mutex_lock(&c->send_lock);
    int rc = proto_send(c->fd, m);
    pthread_mutex_unlock(&c->send_lock);
    return rc;
}

static void send_error_fd(int fd, int32_t code, const char *text)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.error.code = code;
    snprintf(p.error.text, sizeof(p.error.text), "%s", text ? text : "");

    ProtoMessage m;
    if (proto_build(&m, MSG_ERROR, 0, &p) == 0)
        proto_send(fd, &m);
}

static void send_error(ClientSlot *c, int32_t code, const char *text)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.error.code = code;
    snprintf(p.error.text, sizeof(p.error.text), "%s", text ? text : "");

    ProtoMessage m;
    if (proto_build(&m, MSG_ERROR, 0, &p) == 0)
        client_send(c, &m);
}

static void send_ack(ClientSlot *c, uint32_t ref_seq, int32_t code)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.ack.ref_seq = ref_seq;
    p.ack.code = code;

    ProtoMessage m;
    if (proto_build(&m, MSG_ACK, 0, &p) == 0)
        client_send(c, &m);
}

static void broadcast(Server *srv, const ProtoMessage *m, int role_filter)
{
    pthread_mutex_lock(&srv->clients_mutex);
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        ClientSlot *c = &srv->clients[i];
        if (!c->active) continue;
        if (role_filter != 0 && c->role != role_filter) continue;

        if (client_send(c, m) != 0) {
            c->active = 0;
            shutdown(c->fd, SHUT_RDWR);
            io_printf(2,
                "[сервер] клиент #%u (%s) отключён: ошибка отправки\n",
                c->id, c->peer);
        }
    }
    pthread_mutex_unlock(&srv->clients_mutex);
}

// Снимки состояния
static void fill_proto_direction(ProtoDirection *dst, const DirectionSlot *src)
{
    dst->cars_waiting = (int16_t)src->cars_waiting;
    dst->cars_emergency = (int16_t)src->cars_emergency;
    dst->peds_waiting = (int16_t)src->peds_waiting;
    for (int m = 0; m < MANEUVER_COUNT; ++m)
        dst->cars_maneuver[m] = (int16_t)src->cars_maneuver[m];
    dst->cars_arrived = (uint32_t)src->cars_arrived;
    dst->peds_arrived = (uint32_t)src->peds_arrived;
    dst->cars_served = (uint32_t)src->cars_served;
    dst->peds_served = (uint32_t)src->peds_served;
    dst->rejected = (uint32_t)src->rejected;
    dst->wait_ticks = (uint32_t)src->wait_ticks;
}

static void fill_snapshot(Server *srv, ProtoSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));

    SharedState *st = &srv->st;
    model_lock(st);
    snap->tick = (uint32_t)st->stats.ticks;
    snap->arrived = (uint32_t)st->stats.arrived;
    snap->served = (uint32_t)st->stats.served;
    snap->rejected = (uint32_t)st->stats.rejected;
    snap->wait_ticks = (uint32_t)st->stats.wait_ticks;
    snap->phase = (int16_t)st->ctrl.current_phase;
    snap->state = (int16_t)st->ctrl.state;
    snap->transition_left = (int16_t)st->ctrl.transition_left;
    snap->forced_priority = (int16_t)st->ctrl.forced_priority;
    snap->num_directions = (uint8_t)st->cfg.num_directions;

    for (int d = 0; d < MAX_DIRECTIONS; ++d)
        fill_proto_direction(&snap->dirs[d], &st->dirs[d]);
    model_unlock(st);
}

static void fill_stats(Server *srv, ProtoStats *stats)
{
    memset(stats, 0, sizeof(*stats));

    SharedState *st = &srv->st;
    model_lock(st);
    stats->ticks = (uint32_t)st->stats.ticks;
    stats->arrived = (uint32_t)st->stats.arrived;
    stats->served = (uint32_t)st->stats.served;
    stats->rejected = (uint32_t)st->stats.rejected;
    stats->wait_ticks = (uint32_t)st->stats.wait_ticks;
    stats->num_directions = (uint8_t)st->cfg.num_directions;

    for (int d = 0; d < MAX_DIRECTIONS; ++d)
        fill_proto_direction(&stats->dirs[d], &st->dirs[d]);
    model_unlock(st);
}

static int send_snapshot_to_fd(Server *srv, ClientSlot *c)
{
    ProtoSnapshot snap;
    fill_snapshot(srv, &snap);

    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.snapshot = snap;

    ProtoMessage m;
    if (proto_build(&m, MSG_SNAPSHOT, server_next_seq(srv), &p) != 0)
        return -1;
    return client_send(c, &m);
}

static void broadcast_snapshot(Server *srv)
{
    ProtoSnapshot snap;
    fill_snapshot(srv, &snap);

    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.snapshot = snap;

    ProtoMessage m;
    if (proto_build(&m, MSG_SNAPSHOT, server_next_seq(srv), &p) == 0)
        broadcast(srv, &m, ROLE_OBSERVER);
}

static void broadcast_stats(Server *srv)
{
    ProtoStats stats;
    fill_stats(srv, &stats);

    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.stats = stats;

    ProtoMessage m;
    if (proto_build(&m, MSG_STATS, server_next_seq(srv), &p) == 0)
        broadcast(srv, &m, ROLE_OBSERVER);
}

// События модели -> сеть
static void server_event_sink(const ModelEvent *ev, void *ctx)
{
    Server *srv = ctx;

    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.event.tick = (uint32_t)ev->tick;
    p.event.participant_id = ev->participant_id;
    p.event.kind = (uint16_t)ev->kind;
    p.event.direction = (int16_t)ev->direction;
    p.event.maneuver = (int16_t)ev->maneuver;
    p.event.phase = (int16_t)ev->phase;
    p.event.queue_cars = (int16_t)ev->queue_cars;
    p.event.queue_peds = (int16_t)ev->queue_peds;
    p.event.emergency = (uint8_t)(ev->emergency ? 1 : 0);
    p.event.state = (uint8_t)ev->state;

    ProtoMessage m;
    if (proto_build(&m, MSG_EVENT, server_next_seq(srv), &p) == 0)
        broadcast(srv, &m, ROLE_OBSERVER);
}

static void emit_event(Server *srv, ClientSlot *c, ModelEventKind kind,
                       int direction, int maneuver, int emergency,
                       unsigned long tick, int phase, int state,
                       int queue_cars, int queue_peds)
{
    ModelEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.tick = tick;
    ev.participant_id = c->id;
    ev.kind = kind;
    ev.direction = direction;
    ev.maneuver = maneuver;
    ev.phase = phase;
    ev.state = state;
    ev.queue_cars = queue_cars;
    ev.queue_peds = queue_peds;
    ev.emergency = emergency;
    model_emit(&srv->st, &ev);
}

// Потоки модели
static void controller_tick(SharedState *st, void *ctx)
{
    (void)st;
    broadcast_snapshot(ctx);
}

static void *controller_thread(void *arg)
{
    Server *srv = arg;
    controller_run(&srv->st, &srv->st.cfg, controller_tick, srv);
    return NULL;
}

static void *direction_thread(void *arg)
{
    DirectionArg *a = arg;
    direction_run(&a->srv->st, &a->srv->st.cfg, a->dir);
    return NULL;
}

// Обработка сообщений клиентов
static void handle_car_arrival(Server *srv, ClientSlot *c,
                               const ProtoMessage *m,
                               const ProtoCarArrival *car)
{
    SharedState *st = &srv->st;
    const Config *cfg = &st->cfg;

    int dir = car->direction;
    if (dir < 0 || dir >= cfg->num_directions) {
        send_error(c, 400, "недопустимое направление");
        return;
    }
    if (c->direction >= 0 && dir != c->direction) {
        send_error(c, 403, "направление не совпадает с HELLO");
        return;
    }

    int maneuver = car->maneuver;
    if (maneuver < 0 || maneuver >= MANEUVER_COUNT ||
        !cfg->maneuvers[dir][maneuver]) {
        send_error(c, 400, "недопустимый манёвр");
        return;
    }

    int emergency = car->emergency ? 1 : 0;

    model_lock(st);
    int ok = model_add_car(st, dir, emergency, maneuver);
    int qcars = st->dirs[dir].cars_waiting;
    int qpeds = st->dirs[dir].peds_waiting;
    unsigned long tick = st->stats.ticks;
    int phase = st->ctrl.current_phase;
    int state = (int)st->ctrl.state;
    model_unlock(st);

    send_ack(c, m->seq, ok ? 0 : 1);

    if (ok) {
        io_printf(1,
            "[сервер] такт %lu, фаза %d: авто #%u направление %d "
            "(%s%s) в очереди %d/%d\n",
            tick, phase, c->id, dir, model_maneuver_name(maneuver),
            emergency ? ", ЭКСТРЕННЫЙ" : "", qcars, cfg->queue_capacity);
    } else {
        io_printf(2,
            "[сервер] такт %lu: ОТКАЗ авто #%u направление %d (%s): "
            "очередь заполнена\n",
            tick, c->id, dir, model_maneuver_name(maneuver));
    }

    emit_event(srv, c, MODEL_EVENT_CAR_ARRIVED, dir, maneuver, emergency,
               tick, phase, state, qcars, qpeds);
    emit_event(srv, c,
               ok ? MODEL_EVENT_CAR_QUEUED : MODEL_EVENT_CAR_REJECTED,
               dir, maneuver, emergency, tick, phase, state, qcars, qpeds);
}

static void handle_ped_request(Server *srv, ClientSlot *c,
                               const ProtoMessage *m,
                               const ProtoPedRequest *ped)
{
    SharedState *st = &srv->st;
    const Config *cfg = &st->cfg;

    int dir = ped->direction;
    if (dir < 0 || dir >= cfg->num_directions) {
        send_error(c, 400, "недопустимое направление");
        return;
    }
    if (c->direction >= 0 && dir != c->direction) {
        send_error(c, 403, "направление не совпадает с HELLO");
        return;
    }

    model_lock(st);
    int ok = model_add_ped(st, dir);
    int qcars = st->dirs[dir].cars_waiting;
    int qpeds = st->dirs[dir].peds_waiting;
    unsigned long tick = st->stats.ticks;
    int phase = st->ctrl.current_phase;
    int state = (int)st->ctrl.state;
    model_unlock(st);

    send_ack(c, m->seq, ok ? 0 : 1);

    if (ok) {
        io_printf(1,
            "[сервер] такт %lu, фаза %d: пешеход #%u направление %d "
            "нажал кнопку, очередь пешеходов %d/%d\n",
            tick, phase, c->id, dir, qpeds, cfg->queue_capacity);
    } else {
        io_printf(2,
            "[сервер] такт %lu: ОТКАЗ пешеходу #%u направление %d: "
            "очередь заполнена\n",
            tick, c->id, dir);
    }

    emit_event(srv, c, MODEL_EVENT_PED_REQUEST, dir, -1, 0,
               tick, phase, state, qcars, qpeds);
    emit_event(srv, c,
               ok ? MODEL_EVENT_PED_QUEUED : MODEL_EVENT_PED_REJECTED,
               dir, -1, 0, tick, phase, state, qcars, qpeds);
}

static void handle_client_message(Server *srv, ClientSlot *c,
                                  const ProtoMessage *m)
{
    ProtoPayload p;
    if (proto_decode(m, &p) != 0) {
        send_error(c, 400, "не удалось разобрать сообщение");
        return;
    }

    switch ((MsgType)m->type) {
    case MSG_CAR_ARRIVAL:
        if (c->role != ROLE_CAR) {
            send_error(c, 403, "сообщение разрешено только авто");
            return;
        }
        handle_car_arrival(srv, c, m, &p.car_arrival);
        break;

    case MSG_PED_REQUEST:
        if (c->role != ROLE_PED) {
            send_error(c, 403, "сообщение разрешено только пешеходу");
            return;
        }
        handle_ped_request(srv, c, m, &p.ped_request);
        break;

    default:
        send_error(c, 400, "неподдерживаемый тип сообщения");
        break;
    }
}

// Подключение клиента
static int client_handshake(Server *srv, ClientSlot *c)
{
    ProtoMessage m;
    int rc = proto_recv(c->fd, &m);
    if (rc == 0) {
        io_printf(2, "[сервер] клиент #%u отключился до HELLO\n", c->id);
        return -1;
    }
    if (rc < 0) {
        io_printf(2, "[сервер] клиент #%u: ошибка чтения HELLO\n", c->id);
        return -1;
    }
    if (m.type != MSG_HELLO) {
        send_error(c, 400, "ожидалось сообщение HELLO");
        return -1;
    }

    ProtoPayload p;
    if (proto_decode(&m, &p) != 0) {
        send_error(c, 400, "некорректный HELLO");
        return -1;
    }

    int role = p.hello.role;
    if (role != ROLE_CAR && role != ROLE_PED && role != ROLE_OBSERVER) {
        send_error(c, 400, "неизвестная роль клиента");
        return -1;
    }

    int dir = -1;
    if (role == ROLE_CAR || role == ROLE_PED) {
        dir = p.hello.direction;
        if (dir < 0 || dir >= srv->st.cfg.num_directions) {
            send_error(c, 400, "недопустимое направление");
            return -1;
        }
    }

    c->role = role;
    c->direction = dir;
    snprintf(c->name, sizeof(c->name), "%s", p.hello.name);

    ProtoPayload ack;
    memset(&ack, 0, sizeof(ack));
    ack.hello_ack.client_id = c->id;
    ack.hello_ack.num_directions = (uint8_t)srv->st.cfg.num_directions;
    ack.hello_ack.queue_capacity = (uint16_t)srv->st.cfg.queue_capacity;
    ack.hello_ack.tick_ms = (uint32_t)srv->st.cfg.tick_ms;
    ack.hello_ack.phases_count = (uint8_t)srv->st.cfg.phases_count;

    for (int d = 0; d < MAX_DIRECTIONS; ++d)
        for (int man = 0; man < MANEUVER_COUNT; ++man)
            ack.hello_ack.maneuvers[d][man] =
                (uint8_t)(srv->st.cfg.maneuvers[d][man] ? 1 : 0);

    for (int ph = 0; ph < MAX_PHASES; ++ph)
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            ack.hello_ack.phases[ph][d] =
                (uint8_t)(srv->st.cfg.phases[ph][d] ? 1 : 0);

    ProtoMessage out;
    if (proto_build(&out, MSG_HELLO_ACK, m.seq, &ack) != 0 ||
        proto_send(c->fd, &out) != 0) {
        io_printf(2, "[сервер] клиент #%u: не удалось отправить HELLO_ACK\n",
                  c->id);
        return -1;
    }

    return 0;
}

static void *client_thread(void *arg)
{
    ClientArg *ca = arg;
    Server *srv = ca->srv;
    int slot = ca->slot;
    free(ca);

    ClientSlot *c = &srv->clients[slot];

    set_socket_timeout(c->fd, SO_SNDTIMEO, SEND_TIMEOUT_SEC);
    set_socket_timeout(c->fd, SO_RCVTIMEO, HANDSHAKE_TIMEOUT_SEC);

    if (client_handshake(srv, c) != 0)
        goto done;

    set_socket_timeout(c->fd, SO_RCVTIMEO, 0);

    if (c->role == ROLE_OBSERVER) {
        if (send_snapshot_to_fd(srv, c) != 0)
            goto done;
    }

    pthread_mutex_lock(&srv->clients_mutex);
    c->active = 1;
    pthread_mutex_unlock(&srv->clients_mutex);

    if (c->role == ROLE_OBSERVER) {
        io_printf(1, "[сервер] наблюдатель #%u (%s) подключён: %s\n",
                  c->id, c->name[0] ? c->name : "observer", c->peer);
    } else {
        io_printf(1,
            "[сервер] %s #%u (%s, направление %d) подключён: %s\n",
            proto_role_name(c->role), c->id,
            c->name[0] ? c->name : "-", c->direction, c->peer);
    }

    while (!atomic_load_explicit(&srv->stopping, memory_order_relaxed) &&
           !model_stop(&srv->st)) {
        ProtoMessage m;
        int rc = proto_recv(c->fd, &m);
        if (rc == 0) {
            io_printf(1, "[сервер] клиент #%u отключился\n", c->id);
            break;
        }
        if (rc < 0) {
            if (!atomic_load_explicit(&srv->stopping, memory_order_relaxed))
                io_printf(2, "[сервер] клиент #%u: ошибка приёма\n", c->id);
            break;
        }
        handle_client_message(srv, c, &m);
    }

done:
    pthread_mutex_lock(&srv->clients_mutex);
    c->active = 0;
    pthread_mutex_unlock(&srv->clients_mutex);

    close(c->fd);
    c->fd = -1;

    pthread_mutex_lock(&srv->clients_mutex);
    c->in_use = 0;
    pthread_mutex_unlock(&srv->clients_mutex);

    return NULL;
}

// Реестр клиентов
static void client_locks_init(Server *srv)
{
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i)
        pthread_mutex_init(&srv->clients[i].send_lock, NULL);
}

static void client_locks_destroy(Server *srv)
{
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i)
        pthread_mutex_destroy(&srv->clients[i].send_lock);
}

static int registry_add(Server *srv, int fd, const char *peer)
{
    pthread_mutex_lock(&srv->clients_mutex);

    int slot = -1;
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (!srv->clients[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&srv->clients_mutex);
        return -1;
    }

    ClientSlot *c = &srv->clients[slot];
    pthread_t old_thread = c->thread;
    int had_old = c->thread_started;
    c->fd = fd;
    c->role = 0;
    c->direction = -1;
    c->id = srv->next_client_id++;
    c->active = 0;
    c->in_use = 1;
    c->thread_started = 0;
    c->peer[0] = '\0';
    c->name[0] = '\0';
    snprintf(c->peer, sizeof(c->peer), "%s", peer);
    pthread_mutex_unlock(&srv->clients_mutex);
    if (had_old)
        pthread_join(old_thread, NULL);

    return slot;
}

static int registry_start_thread(Server *srv, int slot)
{
    ClientArg *ca = malloc(sizeof(*ca));
    if (!ca) return -1;

    ca->srv = srv;
    ca->slot = slot;

    pthread_mutex_lock(&srv->clients_mutex);
    ClientSlot *c = &srv->clients[slot];
    int rc = pthread_create(&c->thread, NULL, client_thread, ca);
    if (rc == 0) c->thread_started = 1;
    pthread_mutex_unlock(&srv->clients_mutex);

    if (rc != 0) free(ca);
    return rc == 0 ? 0 : -1;
}

static void registry_release(Server *srv, int slot)
{
    pthread_mutex_lock(&srv->clients_mutex);
    srv->clients[slot].active = 0;
    srv->clients[slot].in_use = 0;
    pthread_mutex_unlock(&srv->clients_mutex);
}

// Завершение
static const char *shutdown_reason_name(ShutdownReason reason)
{
    switch (reason) {
    case SHUTDOWN_NORMAL: return "нормальное завершение";
    case SHUTDOWN_SIGNAL: return "прерывание пользователем";
    case SHUTDOWN_ERROR:  return "ошибка";
    default:              return "неизвестно";
    }
}

static void server_shutdown(Server *srv, ShutdownReason reason)
{
    atomic_store_explicit(&srv->stopping, 1, memory_order_relaxed);
    model_request_stop(&srv->st);
    broadcast_stats(srv);
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.shutdown.reason = (uint8_t)reason;
    ProtoMessage m;
    if (proto_build(&m, MSG_SHUTDOWN, server_next_seq(srv), &p) == 0)
        broadcast(srv, &m, 0);

    // Даём клиентам возможность вычитать SHUTDOWN перед закрытием сокета
    usleep(30000);

    pthread_mutex_lock(&srv->clients_mutex);
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        ClientSlot *c = &srv->clients[i];
        if (c->in_use && c->fd >= 0)
            shutdown(c->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&srv->clients_mutex);
    if (srv->controller_started)
        pthread_join(srv->controller_thread, NULL);
    for (int d = 0; d < MAX_DIRECTIONS; ++d)
        if (srv->direction_started[d])
            pthread_join(srv->direction_threads[d], NULL);
    pthread_t threads[SERVER_MAX_CLIENTS];
    int n = 0;
    pthread_mutex_lock(&srv->clients_mutex);
    for (int i = 0; i < SERVER_MAX_CLIENTS; ++i) {
        if (srv->clients[i].thread_started) {
            threads[n++] = srv->clients[i].thread;
            srv->clients[i].thread_started = 0;
        }
    }
    pthread_mutex_unlock(&srv->clients_mutex);
    for (int i = 0; i < n; ++i)
        pthread_join(threads[i], NULL);

    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}

// main
int server_main(int argc, char **argv)
{
    if (argc < 4) {
        io_printf(2,
            "использование: %s <ip> <port> <cfg> [fixed|adaptive]\n",
            argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    const char *port = argv[2];
    const char *cfg_path = argv[3];
    const char *strategy = argc > 4 ? argv[4] : NULL;

    Server srv;
    memset(&srv, 0, sizeof(srv));
    srv.listen_fd = -1;

    Config cfg;
    if (app_load_config(&cfg, cfg_path, strategy) != 0)
        return 1;

    if (model_init(&srv.st, &cfg) != 0)
        return 1;

    app_setup_signals(&srv.st);

    if (pthread_mutex_init(&srv.clients_mutex, NULL) != 0) {
        io_printf(2, "[сервер] не удалось создать мьютекс клиентов\n");
        model_destroy(&srv.st);
        return 1;
    }
    client_locks_init(&srv);

    srv.st.event_sink = server_event_sink;
    srv.st.event_ctx = &srv;
    srv.next_client_id = 1;
    srv.next_seq = 1;

    srv.listen_fd = net_listen(ip, port, SERVER_BACKLOG);
    if (srv.listen_fd < 0) {
        io_printf(2, "[сервер] не удалось открыть порт %s:%s\n",
                  (ip && *ip) ? ip : "*", port);
        client_locks_destroy(&srv);
        pthread_mutex_destroy(&srv.clients_mutex);
        model_destroy(&srv.st);
        return 1;
    }

    io_printf(1, "[сервер] слушаю %s:%s, конфигурация %s\n",
              (ip && *ip) ? ip : "*", port, cfg_path);
    config_print(&srv.st.cfg);

    if (pthread_create(&srv.controller_thread, NULL,
                       controller_thread, &srv) != 0) {
        io_printf(2, "[сервер] не удалось запустить поток контроллера\n");
        close(srv.listen_fd);
        client_locks_destroy(&srv);
        pthread_mutex_destroy(&srv.clients_mutex);
        model_destroy(&srv.st);
        return 1;
    }
    srv.controller_started = 1;

    for (int d = 0; d < srv.st.cfg.num_directions; ++d) {
        srv.dir_args[d].srv = &srv;
        srv.dir_args[d].dir = d;
        if (pthread_create(&srv.direction_threads[d], NULL,
                           direction_thread, &srv.dir_args[d]) == 0) {
            srv.direction_started[d] = 1;
        } else {
            io_printf(2, "[сервер] не удалось запустить поток направления %d\n", d);
        }
    }

    while (!model_stop(&srv.st)) {
        struct pollfd pfd;
        pfd.fd = srv.listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            io_printf(2, "[сервер] poll: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int fd = accept(srv.listen_fd, (struct sockaddr *)&ss, &slen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            io_printf(2, "[сервер] accept: %s\n", strerror(errno));
            continue;
        }

        char peer[64] = "?";
        net_peer_name(fd, peer, sizeof(peer));

        set_socket_timeout(fd, SO_SNDTIMEO, SEND_TIMEOUT_SEC);

        int slot = registry_add(&srv, fd, peer);
        if (slot < 0) {
            send_error_fd(fd, 503, "сервер перегружен");
            close(fd);
            io_printf(2,
                "[сервер] отклонён клиент %s: нет свободных слотов\n", peer);
            continue;
        }

        if (registry_start_thread(&srv, slot) != 0) {
            io_printf(2,
                "[сервер] не удалось создать поток клиента %s\n", peer);
            close(fd);
            registry_release(&srv, slot);
            continue;
        }

        io_printf(1, "[сервер] принято подключение %s (слот %d)\n",
                  peer, slot);
    }

    ShutdownReason reason = app_signal_received()
                                ? SHUTDOWN_SIGNAL
                                : SHUTDOWN_NORMAL;

    io_printf(1, "[сервер] завершение работы: %s\n",
              shutdown_reason_name(reason));

    server_shutdown(&srv, reason);

    model_print_stats(&srv.st);
    app_save_stats(&srv.st, "traffic_sim_4_server.log");

    model_destroy(&srv.st);
    client_locks_destroy(&srv);
    pthread_mutex_destroy(&srv.clients_mutex);

    return 0;
}
