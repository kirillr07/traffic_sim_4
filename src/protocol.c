// protocol.c: сериализация и десериализация сетевых сообщений.
// Все целые числа передаются в сетевом порядке байт (big-endian).

#include "protocol.h"
#include "net.h"

#include <string.h>

// Размер одного блока направления на проводе
#define DIRECTION_SIZE (3 * 2 + MANEUVER_COUNT * 2 + 6 * 4)

// Запись чисел в сетевом порядке байт
static void put_u8(unsigned char *buf, size_t *off, uint8_t v)
{
    buf[(*off)++] = v;
}

static void put_u16(unsigned char *buf, size_t *off, uint16_t v)
{
    buf[(*off)++] = (unsigned char)(v >> 8);
    buf[(*off)++] = (unsigned char)(v & 0xFFu);
}

static void put_u32(unsigned char *buf, size_t *off, uint32_t v)
{
    buf[(*off)++] = (unsigned char)(v >> 24);
    buf[(*off)++] = (unsigned char)((v >> 16) & 0xFFu);
    buf[(*off)++] = (unsigned char)((v >> 8) & 0xFFu);
    buf[(*off)++] = (unsigned char)(v & 0xFFu);
}

static void put_i16(unsigned char *buf, size_t *off, int16_t v)
{
    put_u16(buf, off, (uint16_t)v);
}

static void put_i32(unsigned char *buf, size_t *off, int32_t v)
{
    put_u32(buf, off, (uint32_t)v);
}

// Чтение чисел из сетевого порядка байт
static uint8_t get_u8(const unsigned char *buf, size_t *off)
{
    return buf[(*off)++];
}

static uint16_t get_u16(const unsigned char *buf, size_t *off)
{
    uint16_t v = (uint16_t)((uint16_t)buf[*off] << 8);
    v |= (uint16_t)buf[*off + 1];
    *off += 2;
    return v;
}

static uint32_t get_u32(const unsigned char *buf, size_t *off)
{
    uint32_t v = (uint32_t)buf[*off] << 24;
    v |= (uint32_t)buf[*off + 1] << 16;
    v |= (uint32_t)buf[*off + 2] << 8;
    v |= (uint32_t)buf[*off + 3];
    *off += 4;
    return v;
}

static int16_t get_i16(const unsigned char *buf, size_t *off)
{
    return (int16_t)get_u16(buf, off);
}

static int32_t get_i32(const unsigned char *buf, size_t *off)
{
    return (int32_t)get_u32(buf, off);
}

// Блоки направлений
static void put_direction(unsigned char *buf, size_t *off,
                          const ProtoDirection *d)
{
    put_i16(buf, off, d->cars_waiting);
    put_i16(buf, off, d->cars_emergency);
    put_i16(buf, off, d->peds_waiting);
    for (int m = 0; m < MANEUVER_COUNT; ++m)
        put_i16(buf, off, d->cars_maneuver[m]);
    put_u32(buf, off, d->cars_arrived);
    put_u32(buf, off, d->peds_arrived);
    put_u32(buf, off, d->cars_served);
    put_u32(buf, off, d->peds_served);
    put_u32(buf, off, d->rejected);
    put_u32(buf, off, d->wait_ticks);
}

static void get_direction(const unsigned char *buf, size_t *off,
                          ProtoDirection *d)
{
    d->cars_waiting = get_i16(buf, off);
    d->cars_emergency = get_i16(buf, off);
    d->peds_waiting = get_i16(buf, off);
    for (int m = 0; m < MANEUVER_COUNT; ++m)
        d->cars_maneuver[m] = get_i16(buf, off);
    d->cars_arrived = get_u32(buf, off);
    d->peds_arrived = get_u32(buf, off);
    d->cars_served = get_u32(buf, off);
    d->peds_served = get_u32(buf, off);
    d->rejected = get_u32(buf, off);
    d->wait_ticks = get_u32(buf, off);
}

// Ожидаемый размер payload
static size_t expected_size(MsgType type)
{
    switch (type) {
    case MSG_HELLO:
        return 2 + PROTO_NAME_LEN;
    case MSG_HELLO_ACK:
        return 4 + 1 + 2 + 4 +
               MAX_DIRECTIONS * MANEUVER_COUNT + 1 +
               MAX_PHASES * MAX_DIRECTIONS;
    case MSG_CAR_ARRIVAL:
        return 4 + 3;
    case MSG_PED_REQUEST:
        return 4 + 1;
    case MSG_EVENT:
        return 4 + 4 + 2 + 5 * 2 + 2;
    case MSG_SNAPSHOT:
        return 5 * 4 + 4 * 2 + 1 + MAX_DIRECTIONS * DIRECTION_SIZE;
    case MSG_STATS:
        return 5 * 4 + 1 + MAX_DIRECTIONS * DIRECTION_SIZE;
    case MSG_SHUTDOWN:
        return 1;
    case MSG_ACK:
        return 4 + 4;
    case MSG_ERROR:
        return 4 + PROTO_ERROR_LEN;
    default:
        return (size_t)-1;
    }
}

// Публичный API
void proto_msg_init(ProtoMessage *m, MsgType type, uint32_t seq)
{
    memset(m, 0, sizeof(*m));
    m->magic = PROTO_MAGIC;
    m->version = PROTO_VERSION;
    m->type = (uint16_t)type;
    m->seq = seq;
    m->length = 0;
}

int proto_encode(ProtoMessage *m, const ProtoPayload *p)
{
    if (!m || !p) return -1;

    size_t off = 0;
    unsigned char *buf = m->payload;

    switch ((MsgType)m->type) {
    case MSG_HELLO:
        put_u8(buf, &off, p->hello.role);
        put_u8(buf, &off, p->hello.direction);
        for (int i = 0; i < PROTO_NAME_LEN; ++i)
            put_u8(buf, &off, (uint8_t)p->hello.name[i]);
        break;

    case MSG_HELLO_ACK:
        put_u32(buf, &off, p->hello_ack.client_id);
        put_u8(buf, &off, p->hello_ack.num_directions);
        put_u16(buf, &off, p->hello_ack.queue_capacity);
        put_u32(buf, &off, p->hello_ack.tick_ms);
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            for (int m = 0; m < MANEUVER_COUNT; ++m)
                put_u8(buf, &off, p->hello_ack.maneuvers[d][m]);
        put_u8(buf, &off, p->hello_ack.phases_count);
        for (int ph = 0; ph < MAX_PHASES; ++ph)
            for (int d = 0; d < MAX_DIRECTIONS; ++d)
                put_u8(buf, &off, p->hello_ack.phases[ph][d]);
        break;

    case MSG_CAR_ARRIVAL:
        put_u32(buf, &off, p->car_arrival.client_id);
        put_u8(buf, &off, p->car_arrival.direction);
        put_u8(buf, &off, p->car_arrival.maneuver);
        put_u8(buf, &off, p->car_arrival.emergency);
        break;

    case MSG_PED_REQUEST:
        put_u32(buf, &off, p->ped_request.client_id);
        put_u8(buf, &off, p->ped_request.direction);
        break;

    case MSG_EVENT:
        put_u32(buf, &off, p->event.tick);
        put_u32(buf, &off, p->event.participant_id);
        put_u16(buf, &off, p->event.kind);
        put_i16(buf, &off, p->event.direction);
        put_i16(buf, &off, p->event.maneuver);
        put_i16(buf, &off, p->event.phase);
        put_i16(buf, &off, p->event.queue_cars);
        put_i16(buf, &off, p->event.queue_peds);
        put_u8(buf, &off, p->event.emergency);
        put_u8(buf, &off, p->event.state);
        break;

    case MSG_SNAPSHOT:
        put_u32(buf, &off, p->snapshot.tick);
        put_u32(buf, &off, p->snapshot.arrived);
        put_u32(buf, &off, p->snapshot.served);
        put_u32(buf, &off, p->snapshot.rejected);
        put_u32(buf, &off, p->snapshot.wait_ticks);
        put_i16(buf, &off, p->snapshot.phase);
        put_i16(buf, &off, p->snapshot.state);
        put_i16(buf, &off, p->snapshot.transition_left);
        put_i16(buf, &off, p->snapshot.forced_priority);
        put_u8(buf, &off, p->snapshot.num_directions);
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            put_direction(buf, &off, &p->snapshot.dirs[d]);
        break;

    case MSG_STATS:
        put_u32(buf, &off, p->stats.ticks);
        put_u32(buf, &off, p->stats.arrived);
        put_u32(buf, &off, p->stats.served);
        put_u32(buf, &off, p->stats.rejected);
        put_u32(buf, &off, p->stats.wait_ticks);
        put_u8(buf, &off, p->stats.num_directions);
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            put_direction(buf, &off, &p->stats.dirs[d]);
        break;

    case MSG_SHUTDOWN:
        put_u8(buf, &off, p->shutdown.reason);
        break;

    case MSG_ACK:
        put_u32(buf, &off, p->ack.ref_seq);
        put_i32(buf, &off, p->ack.code);
        break;

    case MSG_ERROR:
        put_i32(buf, &off, p->error.code);
        for (int i = 0; i < PROTO_ERROR_LEN; ++i)
            put_u8(buf, &off, (uint8_t)p->error.text[i]);
        break;

    default:
        return -1;
    }

    if (off > PROTO_MAX_PAYLOAD) return -1;
    m->length = (uint32_t)off;
    return 0;
}

int proto_build(ProtoMessage *m, MsgType type, uint32_t seq,
                const ProtoPayload *p)
{
    if (!m) return -1;
    proto_msg_init(m, type, seq);
    return proto_encode(m, p);
}

int proto_decode(const ProtoMessage *m, ProtoPayload *p)
{
    if (!m || !p) return -1;
    if (m->magic != PROTO_MAGIC) return -1;
    if (m->version != PROTO_VERSION) return -1;
    if (m->length > PROTO_MAX_PAYLOAD) return -1;

    MsgType type = (MsgType)m->type;
    size_t want = expected_size(type);
    if (want == (size_t)-1 || m->length != want) return -1;

    memset(p, 0, sizeof(*p));

    size_t off = 0;
    const unsigned char *buf = m->payload;

    switch (type) {
    case MSG_HELLO:
        p->hello.role = get_u8(buf, &off);
        p->hello.direction = get_u8(buf, &off);
        for (int i = 0; i < PROTO_NAME_LEN; ++i)
            p->hello.name[i] = (char)get_u8(buf, &off);
        p->hello.name[PROTO_NAME_LEN - 1] = '\0';
        break;

    case MSG_HELLO_ACK:
        p->hello_ack.client_id = get_u32(buf, &off);
        p->hello_ack.num_directions = get_u8(buf, &off);
        p->hello_ack.queue_capacity = get_u16(buf, &off);
        p->hello_ack.tick_ms = get_u32(buf, &off);
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            for (int m = 0; m < MANEUVER_COUNT; ++m)
                p->hello_ack.maneuvers[d][m] = get_u8(buf, &off);
        p->hello_ack.phases_count = get_u8(buf, &off);
        for (int ph = 0; ph < MAX_PHASES; ++ph)
            for (int d = 0; d < MAX_DIRECTIONS; ++d)
                p->hello_ack.phases[ph][d] = get_u8(buf, &off);
        break;

    case MSG_CAR_ARRIVAL:
        p->car_arrival.client_id = get_u32(buf, &off);
        p->car_arrival.direction = get_u8(buf, &off);
        p->car_arrival.maneuver = get_u8(buf, &off);
        p->car_arrival.emergency = get_u8(buf, &off);
        break;

    case MSG_PED_REQUEST:
        p->ped_request.client_id = get_u32(buf, &off);
        p->ped_request.direction = get_u8(buf, &off);
        break;

    case MSG_EVENT:
        p->event.tick = get_u32(buf, &off);
        p->event.participant_id = get_u32(buf, &off);
        p->event.kind = get_u16(buf, &off);
        p->event.direction = get_i16(buf, &off);
        p->event.maneuver = get_i16(buf, &off);
        p->event.phase = get_i16(buf, &off);
        p->event.queue_cars = get_i16(buf, &off);
        p->event.queue_peds = get_i16(buf, &off);
        p->event.emergency = get_u8(buf, &off);
        p->event.state = get_u8(buf, &off);
        break;

    case MSG_SNAPSHOT:
        p->snapshot.tick = get_u32(buf, &off);
        p->snapshot.arrived = get_u32(buf, &off);
        p->snapshot.served = get_u32(buf, &off);
        p->snapshot.rejected = get_u32(buf, &off);
        p->snapshot.wait_ticks = get_u32(buf, &off);
        p->snapshot.phase = get_i16(buf, &off);
        p->snapshot.state = get_i16(buf, &off);
        p->snapshot.transition_left = get_i16(buf, &off);
        p->snapshot.forced_priority = get_i16(buf, &off);
        p->snapshot.num_directions = get_u8(buf, &off);
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            get_direction(buf, &off, &p->snapshot.dirs[d]);
        break;

    case MSG_STATS:
        p->stats.ticks = get_u32(buf, &off);
        p->stats.arrived = get_u32(buf, &off);
        p->stats.served = get_u32(buf, &off);
        p->stats.rejected = get_u32(buf, &off);
        p->stats.wait_ticks = get_u32(buf, &off);
        p->stats.num_directions = get_u8(buf, &off);
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            get_direction(buf, &off, &p->stats.dirs[d]);
        break;

    case MSG_SHUTDOWN:
        p->shutdown.reason = get_u8(buf, &off);
        break;

    case MSG_ACK:
        p->ack.ref_seq = get_u32(buf, &off);
        p->ack.code = get_i32(buf, &off);
        break;

    case MSG_ERROR:
        p->error.code = get_i32(buf, &off);
        for (int i = 0; i < PROTO_ERROR_LEN; ++i)
            p->error.text[i] = (char)get_u8(buf, &off);
        p->error.text[PROTO_ERROR_LEN - 1] = '\0';
        break;

    default:
        return -1;
    }

    return off == m->length ? 0 : -1;
}

int proto_send(int fd, const ProtoMessage *m)
{
    if (!m) return -1;
    if (m->magic != PROTO_MAGIC || m->version != PROTO_VERSION) return -1;
    if (m->length > PROTO_MAX_PAYLOAD) return -1;

    size_t want = expected_size((MsgType)m->type);
    if (want == (size_t)-1 || m->length != want) return -1;

    unsigned char hdr[PROTO_HEADER_SIZE];
    size_t off = 0;
    put_u32(hdr, &off, m->magic);
    put_u16(hdr, &off, m->version);
    put_u16(hdr, &off, m->type);
    put_u32(hdr, &off, m->seq);
    put_u32(hdr, &off, m->length);

    if (net_send_all(fd, hdr, sizeof(hdr)) != 0) return -1;
    if (m->length > 0 &&
        net_send_all(fd, m->payload, (size_t)m->length) != 0)
        return -1;

    return 0;
}

int proto_recv(int fd, ProtoMessage *m)
{
    if (!m) return -1;

    unsigned char hdr[PROTO_HEADER_SIZE];
    ssize_t n = net_recv_all(fd, hdr, sizeof(hdr));
    if (n == 0) return 0;
    if (n < 0) return -1;

    size_t off = 0;
    m->magic = get_u32(hdr, &off);
    m->version = get_u16(hdr, &off);
    m->type = get_u16(hdr, &off);
    m->seq = get_u32(hdr, &off);
    m->length = get_u32(hdr, &off);

    if (m->magic != PROTO_MAGIC || m->version != PROTO_VERSION) return -1;
    if (m->length > PROTO_MAX_PAYLOAD) return -1;

    if (m->length > 0) {
        n = net_recv_all(fd, m->payload, (size_t)m->length);
        if (n != (ssize_t)m->length) return -1;
    }

    return 1;
}

const char *proto_type_name(int type)
{
    switch ((MsgType)type) {
    case MSG_HELLO:       return "HELLO";
    case MSG_HELLO_ACK:   return "HELLO_ACK";
    case MSG_CAR_ARRIVAL: return "CAR_ARRIVAL";
    case MSG_PED_REQUEST: return "PED_REQUEST";
    case MSG_EVENT:       return "EVENT";
    case MSG_SNAPSHOT:    return "SNAPSHOT";
    case MSG_STATS:       return "STATS";
    case MSG_SHUTDOWN:    return "SHUTDOWN";
    case MSG_ACK:         return "ACK";
    case MSG_ERROR:       return "ERROR";
    default:              return "UNKNOWN";
    }
}

const char *proto_role_name(int role)
{
    switch ((ClientRole)role) {
    case ROLE_CAR:      return "car";
    case ROLE_PED:      return "ped";
    case ROLE_OBSERVER: return "observer";
    default:            return "unknown";
    }
}

const char *proto_event_name(int kind)
{
    switch ((EventKind)kind) {
    case EVENT_CAR_ARRIVED:       return "car_arrived";
    case EVENT_CAR_QUEUED:        return "car_queued";
    case EVENT_CAR_REJECTED:      return "car_rejected";
    case EVENT_CAR_PASSED:        return "car_passed";
    case EVENT_PED_REQUEST:       return "ped_request";
    case EVENT_PED_QUEUED:        return "ped_queued";
    case EVENT_PED_REJECTED:      return "ped_rejected";
    case EVENT_PED_PASSED:        return "ped_passed";
    case EVENT_PHASE_START:       return "phase_start";
    case EVENT_PHASE_END:         return "phase_end";
    case EVENT_TRANSITION_START:  return "transition_start";
    case EVENT_TRANSITION_END:    return "transition_end";
    case EVENT_EMERGENCY_PRIORITY:return "emergency_priority";
    case EVENT_SERVER_START:      return "server_start";
    case EVENT_SERVER_STOP:       return "server_stop";
    default:                      return "unknown";
    }
}
