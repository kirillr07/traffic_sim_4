#ifndef CLIENT_H
#define CLIENT_H

#include "config.h"
#include "protocol.h"

#include <stdint.h>

// Клиентское соединение с сервером
typedef struct {
    int fd;
    uint32_t client_id;
    ClientRole role;
    int direction;
    char name[PROTO_NAME_LEN];
    char ip[64];
    char port[16];
    uint32_t next_seq;
    ProtoHelloAck cfg; // параметры, переданные сервером в HELLO_ACK

    int auto_reconnect;     // 1 — автоматически переподключаться при обрыве
    int retry_interval_ms;  // пауза между попытками подключения (мс)
    int max_retries;        // макс. число попыток (-1 — без ограничения, пока нет сигнала)
} ClientConnection;

// Инициализация структуры клиента (без открытия сокета)
void client_init(ClientConnection *cli, const char *ip, const char *port,
                 ClientRole role, int direction, const char *name);

// Подключение к серверу с повторными попытками и handshake (HELLO -> HELLO_ACK)
// Возвращает 0 при успехе, -1 при ошибке / получении сигнала остановки.
int client_connect_with_retry(ClientConnection *cli, int max_retries,
                              int retry_interval_ms);

// Проверка активности дескриптора соединения
int client_is_connected(const ClientConnection *cli);

// Закрыть соединение
void client_disconnect(ClientConnection *cli);

// Переподключение (если сокет закрылся или оборвался)
int client_reconnect_if_needed(ClientConnection *cli);

// Приём входящего сообщения с таймаутом (мс).
// timeout_ms < 0: блокирующее чтение; 0: неблокирующий опрос; > 0: с таймаутом.
// Возвращает:
//   1  — сообщение получено и успешно декодировано в payload;
//   0  — таймаут (сообщений нет);
//   2  — получен MSG_SHUTDOWN (сервер завершает работу);
//  -1  — ошибка соединения / EOF (сокет закрыт).
int client_recv_message_timeout(ClientConnection *cli, ProtoMessage *m,
                                ProtoPayload *p, int timeout_ms);

// Отправка заявки автомобиля (для ROLE_CAR).
// Возвращает:
//   0  — успешно отправлено и получен ACK от сервера;
//        в *out_accepted кладётся 1 (поставлен в очередь) или 0 (отказ);
//   1  — получен MSG_SHUTDOWN от сервера;
//  -1  — ошибка сети / разрыв соединения.
int client_send_car(ClientConnection *cli, int maneuver, int emergency,
                    int *out_accepted);

// Отправка вызова пешехода (для ROLE_PED).
// Возвращает:
//   0  — успешно отправлено и получен ACK;
//        в *out_accepted кладётся 1 (принят в очередь) или 0 (отказ);
//   1  — получен MSG_SHUTDOWN;
//  -1  — ошибка сети / разрыв соединения.
int client_send_ped(ClientConnection *cli, int *out_accepted);

// Выбрать манёвр из числа разрешённых сервером для направления клиента.
// Если для направления манёвры не заданы, возвращает MANEUVER_STRAIGHT.
int client_choose_maneuver(const ClientConnection *cli);

// Проверить, разрешён ли манёвр сервером
int client_is_maneuver_allowed(const ClientConnection *cli, int maneuver);

#endif /* CLIENT_H */
