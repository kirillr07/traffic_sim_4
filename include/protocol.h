#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "config.h"

// Версия и ограничения протокола
#define PROTO_MAGIC       0x54533431u
#define PROTO_VERSION     1
#define PROTO_HEADER_SIZE 16
#define PROTO_MAX_PAYLOAD 4096
#define PROTO_NAME_LEN    32
#define PROTO_ERROR_LEN   96

// Типы сообщений
typedef enum {
    MSG_HELLO = 1,       // клиент -> сервер: роль, направление, имя
    MSG_HELLO_ACK,       // сервер -> клиент: id и параметры конфигурации
    MSG_CAR_ARRIVAL,     // клиент-авто -> сервер: прибытие автомобиля
    MSG_PED_REQUEST,     // клиент-пешеход -> сервер: запрос перехода
    MSG_EVENT,           // сервер -> наблюдатели: событие модели
    MSG_SNAPSHOT,        // сервер -> наблюдатели: снимок состояния
    MSG_STATS,           // сервер -> наблюдатели: итоговая статистика
    MSG_SHUTDOWN,        // сервер -> клиенты: корректное завершение
    MSG_ACK,             // подтверждение обработки сообщения
    MSG_ERROR            // ошибка протокола или запроса
} MsgType;

// Роли сетевых клиентов
typedef enum {
    ROLE_CAR = 1,
    ROLE_PED = 2,
    ROLE_OBSERVER = 3
} ClientRole;

// Виды событий модели
typedef enum {
    EVENT_CAR_ARRIVED = 1,
    EVENT_CAR_QUEUED,
    EVENT_CAR_REJECTED,
    EVENT_CAR_PASSED,
    EVENT_PED_REQUEST,
    EVENT_PED_QUEUED,
    EVENT_PED_REJECTED,
    EVENT_PED_PASSED,
    EVENT_PHASE_START,
    EVENT_PHASE_END,
    EVENT_TRANSITION_START,
    EVENT_TRANSITION_END,
    EVENT_EMERGENCY_PRIORITY,
    EVENT_SERVER_START,
    EVENT_SERVER_STOP
} EventKind;

// Причины завершения сервера
typedef enum {
    SHUTDOWN_NORMAL = 0,
    SHUTDOWN_SIGNAL = 1,
    SHUTDOWN_ERROR  = 2
} ShutdownReason;

typedef struct {
    uint8_t role;
    uint8_t direction;
    char    name[PROTO_NAME_LEN];
} ProtoHello;

typedef struct {
    uint32_t client_id;
    uint8_t  num_directions;
    uint16_t queue_capacity;
    uint32_t tick_ms;
    uint8_t  maneuvers[MAX_DIRECTIONS][MANEUVER_COUNT];
    uint8_t  phases_count;
    uint8_t  phases[MAX_PHASES][MAX_DIRECTIONS];
} ProtoHelloAck;

typedef struct {
    uint32_t client_id;
    uint8_t  direction;
    uint8_t  maneuver;
    uint8_t  emergency;
} ProtoCarArrival;

typedef struct {
    uint32_t client_id;
    uint8_t  direction;
} ProtoPedRequest;

typedef struct {
    uint32_t tick;
    uint32_t participant_id;
    uint16_t kind;
    int16_t  direction;
    int16_t  maneuver;
    int16_t  phase;
    int16_t  queue_cars;
    int16_t  queue_peds;
    uint8_t  emergency;
    uint8_t  state;
} ProtoEvent;

typedef struct {
    int16_t  cars_waiting;
    int16_t  cars_emergency;
    int16_t  peds_waiting;
    int16_t  cars_maneuver[MANEUVER_COUNT];
    uint32_t cars_arrived;
    uint32_t peds_arrived;
    uint32_t cars_served;
    uint32_t peds_served;
    uint32_t rejected;
    uint32_t wait_ticks;
} ProtoDirection;

typedef struct {
    uint32_t tick;
    uint32_t arrived;
    uint32_t served;
    uint32_t rejected;
    uint32_t wait_ticks;
    int16_t  phase;
    int16_t  state;
    int16_t  transition_left;
    int16_t  forced_priority;
    uint8_t  num_directions;
    ProtoDirection dirs[MAX_DIRECTIONS];
} ProtoSnapshot;

typedef struct {
    uint32_t ticks;
    uint32_t arrived;
    uint32_t served;
    uint32_t rejected;
    uint32_t wait_ticks;
    uint8_t  num_directions;
    ProtoDirection dirs[MAX_DIRECTIONS];
} ProtoStats;

typedef struct {
    uint8_t reason;
} ProtoShutdown;

typedef struct {
    uint32_t ref_seq;
    int32_t  code;
} ProtoAck;

typedef struct {
    int32_t code;
    char    text[PROTO_ERROR_LEN];
} ProtoError;

typedef union {
    ProtoHello      hello;
    ProtoHelloAck   hello_ack;
    ProtoCarArrival car_arrival;
    ProtoPedRequest ped_request;
    ProtoEvent      event;
    ProtoSnapshot   snapshot;
    ProtoStats      stats;
    ProtoShutdown   shutdown;
    ProtoAck        ack;
    ProtoError      error;
} ProtoPayload;

// Сообщение целиком: заголовок + сырая полезная нагрузка
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t seq;
    uint32_t length;
    unsigned char payload[PROTO_MAX_PAYLOAD];
} ProtoMessage;

// Инициализация заголовка сообщения
void proto_msg_init(ProtoMessage *m, MsgType type, uint32_t seq);

// Собрать сообщение: init + сериализация payload.
// Возвращает 0 при успехе, -1 при ошибке.
int proto_build(ProtoMessage *m, MsgType type, uint32_t seq,
                const ProtoPayload *p);

// Сериализовать payload в уже инициализированное сообщение
int proto_encode(ProtoMessage *m, const ProtoPayload *p);

// Разобрать payload сообщения. Возвращает 0 при успехе, -1 при ошибке.
int proto_decode(const ProtoMessage *m, ProtoPayload *p);

// Отправка/приём одного сообщения через TCP.
// proto_recv: 1 — сообщение получено, 0 — корректный EOF, -1 — ошибка.
int proto_send(int fd, const ProtoMessage *m);
int proto_recv(int fd, ProtoMessage *m);

// Имена для логов и отладочного вывода
const char *proto_type_name(int type);
const char *proto_role_name(int role);
const char *proto_event_name(int kind);

#endif /* PROTOCOL_H */
