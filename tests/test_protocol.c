// test_protocol.c: проверка сериализации и round-trip через socketpair.

#include "io.h"
#include "model.h"
#include "net.h"
#include "protocol.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

#define FAILF(label, what)                                                 \
    do {                                                                   \
        io_printf(2, "  FAIL: %s: %s\n", (label), (what));                 \
        ++failures;                                                        \
    } while (0)

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            io_printf(2, "  FAIL: %s\n", (msg));                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

static int roundtrip(const ProtoMessage *out, ProtoMessage *in,
                     const char *label)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAILF(label, "socketpair");
        return 0;
    }

    int ok = 1;
    if (proto_send(sv[0], out) != 0) {
        FAILF(label, "proto_send");
        ok = 0;
    }

    if (ok) {
        int rc = proto_recv(sv[1], in);
        if (rc != 1) {
            io_printf(2, "  FAIL: %s: proto_recv rc=%d\n", label, rc);
            ++failures;
            ok = 0;
        }
    }

    close(sv[0]);
    close(sv[1]);
    return ok;
}

#define TEST_MESSAGE(label, msg_type, member)                              \
    do {                                                                   \
        ProtoMessage out;                                                  \
        ProtoMessage in;                                                   \
        ProtoPayload dec;                                                  \
        if (proto_build(&out, (msg_type), 1, &p) != 0) {                   \
            FAILF((label), "build");                                       \
        } else if (roundtrip(&out, &in, (label))) {                        \
            if (in.type != (uint16_t)(msg_type))                           \
                FAILF((label), "type");                                    \
            if (in.length != out.length)                                   \
                FAILF((label), "length");                                  \
            if (proto_decode(&in, &dec) != 0)                              \
                FAILF((label), "decode");                                  \
            else if (memcmp(&dec.member, &p.member,                         \
                            sizeof(p.member)) != 0)                        \
                FAILF((label), "payload");                                 \
        }                                                                  \
    } while (0)

static void test_hello(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.hello.role = ROLE_CAR;
    p.hello.direction = 2;
    strcpy(p.hello.name, "car-2");
    TEST_MESSAGE("hello", MSG_HELLO, hello);
}

static void test_hello_ack(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.hello_ack.client_id = 42;
    p.hello_ack.num_directions = 4;
    p.hello_ack.queue_capacity = 10;
    p.hello_ack.tick_ms = 15;
    p.hello_ack.maneuvers[0][MANEUVER_STRAIGHT] = 1;
    p.hello_ack.maneuvers[1][MANEUVER_RIGHT] = 1;
    p.hello_ack.maneuvers[3][MANEUVER_LEFT] = 1;
    p.hello_ack.phases_count = 2;
    p.hello_ack.phases[0][0] = 1;
    p.hello_ack.phases[0][2] = 1;
    p.hello_ack.phases[1][1] = 1;
    p.hello_ack.phases[1][3] = 1;
    TEST_MESSAGE("hello_ack", MSG_HELLO_ACK, hello_ack);
}

static void test_car_arrival(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.car_arrival.client_id = 7;
    p.car_arrival.direction = 3;
    p.car_arrival.maneuver = MANEUVER_LEFT;
    p.car_arrival.emergency = 1;
    TEST_MESSAGE("car_arrival", MSG_CAR_ARRIVAL, car_arrival);
}

static void test_ped_request(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.ped_request.client_id = 9;
    p.ped_request.direction = 1;
    TEST_MESSAGE("ped_request", MSG_PED_REQUEST, ped_request);
}

static void test_event(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.event.tick = 123;
    p.event.participant_id = 55;
    p.event.kind = EVENT_CAR_PASSED;
    p.event.direction = 2;
    p.event.maneuver = MANEUVER_RIGHT;
    p.event.phase = 1;
    p.event.queue_cars = 4;
    p.event.queue_peds = 2;
    p.event.emergency = 1;
    p.event.state = PHASE_GREEN;
    TEST_MESSAGE("event", MSG_EVENT, event);
}

static void fill_direction(ProtoDirection *d, int seed)
{
    d->cars_waiting = (int16_t)(seed + 1);
    d->cars_emergency = (int16_t)(seed % 2);
    d->peds_waiting = (int16_t)(seed + 2);
    d->cars_maneuver[MANEUVER_STRAIGHT] = (int16_t)(seed + 3);
    d->cars_maneuver[MANEUVER_RIGHT] = (int16_t)(seed + 4);
    d->cars_maneuver[MANEUVER_LEFT] = (int16_t)(seed + 5);
    d->cars_arrived = (uint32_t)(100 + seed);
    d->peds_arrived = (uint32_t)(200 + seed);
    d->cars_served = (uint32_t)(300 + seed);
    d->peds_served = (uint32_t)(400 + seed);
    d->rejected = (uint32_t)(500 + seed);
    d->wait_ticks = (uint32_t)(600 + seed);
}

static void test_snapshot(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.snapshot.tick = 1000;
    p.snapshot.arrived = 50;
    p.snapshot.served = 40;
    p.snapshot.rejected = 5;
    p.snapshot.wait_ticks = 1234;
    p.snapshot.phase = 1;
    p.snapshot.state = PHASE_TRANSITION;
    p.snapshot.transition_left = 2;
    p.snapshot.forced_priority = 1;
    p.snapshot.num_directions = 4;
    for (int d = 0; d < MAX_DIRECTIONS; ++d)
        fill_direction(&p.snapshot.dirs[d], d);
    TEST_MESSAGE("snapshot", MSG_SNAPSHOT, snapshot);
}

static void test_stats(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.stats.ticks = 777;
    p.stats.arrived = 100;
    p.stats.served = 90;
    p.stats.rejected = 10;
    p.stats.wait_ticks = 5000;
    p.stats.num_directions = 4;
    for (int d = 0; d < MAX_DIRECTIONS; ++d)
        fill_direction(&p.stats.dirs[d], d + 10);
    TEST_MESSAGE("stats", MSG_STATS, stats);
}

static void test_shutdown(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.shutdown.reason = SHUTDOWN_SIGNAL;
    TEST_MESSAGE("shutdown", MSG_SHUTDOWN, shutdown);
}

static void test_ack(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.ack.ref_seq = 99;
    p.ack.code = -3;
    TEST_MESSAGE("ack", MSG_ACK, ack);
}

static void test_error(void)
{
    ProtoPayload p;
    memset(&p, 0, sizeof(p));
    p.error.code = 400;
    strcpy(p.error.text, "недопустимое направление");
    TEST_MESSAGE("error", MSG_ERROR, error);
}

static void test_wire_header(void)
{
    ProtoPayload p;
    ProtoMessage m;
    memset(&p, 0, sizeof(p));
    p.ack.ref_seq = 0x01020304u;
    p.ack.code = -3;

    CHECK(proto_build(&m, MSG_ACK, 0x01020304u, &p) == 0,
          "wire: build ack");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAILF("wire", "socketpair");
        return;
    }

    unsigned char raw[PROTO_HEADER_SIZE + 8];
    if (proto_send(sv[0], &m) != 0) {
        FAILF("wire", "proto_send");
    } else if (net_recv_all(sv[1], raw, sizeof(raw)) !=
               (ssize_t)sizeof(raw)) {
        FAILF("wire", "net_recv_all");
    } else {
        // Заголовок: magic, version, type, seq, length — big-endian.
        // Payload ACK: ref_seq = 0x01020304, code = -3.
        const unsigned char want[] = {
            0x54, 0x53, 0x34, 0x31,
            0x00, 0x01,
            0x00, 0x09,
            0x01, 0x02, 0x03, 0x04,
            0x00, 0x00, 0x00, 0x08,
            0x01, 0x02, 0x03, 0x04,
            0xFF, 0xFF, 0xFF, 0xFD
        };
        CHECK(memcmp(raw, want, sizeof(want)) == 0, "wire: bytes");
    }

    close(sv[0]);
    close(sv[1]);
}

static void test_invalid(void)
{
    ProtoPayload p;
    ProtoMessage m;
    memset(&p, 0, sizeof(p));
    p.ack.ref_seq = 1;
    p.ack.code = 0;

    CHECK(proto_build(&m, MSG_ACK, 1, &p) == 0, "invalid: build ack");

    ProtoMessage bad = m;
    bad.magic = 0;
    CHECK(proto_decode(&bad, &p) == -1, "invalid: magic decode");
    CHECK(proto_send(1, &bad) == -1, "invalid: magic send");

    bad = m;
    bad.length = 7;
    CHECK(proto_decode(&bad, &p) == -1, "invalid: length decode");

    bad = m;
    bad.version = PROTO_VERSION + 1;
    CHECK(proto_decode(&bad, &p) == -1, "invalid: version decode");

    bad = m;
    bad.type = 999;
    CHECK(proto_encode(&bad, &p) == -1, "invalid: unknown type encode");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        close(sv[0]);
        ProtoMessage in;
        CHECK(proto_recv(sv[1], &in) == 0, "invalid: recv EOF");
        close(sv[1]);
    }
}

int main(void)
{
    io_printf(1, "protocol: запуск тестов\n");

    test_hello();
    test_hello_ack();
    test_car_arrival();
    test_ped_request();
    test_event();
    test_snapshot();
    test_stats();
    test_shutdown();
    test_ack();
    test_error();
    test_wire_header();
    test_invalid();

    if (failures == 0) {
        io_printf(1, "protocol: все тесты пройдены\n");
        return 0;
    }

    io_printf(2, "protocol: ошибок %d\n", failures);
    return 1;
}
