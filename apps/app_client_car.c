// app_client_car.c: сетевой клиент-генератор автомобилей для одного направления

#include "app.h"
#include "client.h"
#include "io.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        io_printf(2, "использование: %s <ip> <port> <направление> [cfg]\n",
                  argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    const char *port = argv[2];
    int dir = atoi(argv[3]);
    const char *cfg_path = argc > 4 ? argv[4] : NULL;

    if (dir < 0 || dir >= MAX_DIRECTIONS) {
        io_printf(2, "ошибка: недопустимый номер направления %d (0..%d)\n",
                  dir, MAX_DIRECTIONS - 1);
        return 1;
    }

    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.car_arrival_percent = 35;
    cfg.emergency_percent = 2;
    cfg.tick_ms = 50;

    if (cfg_path && *cfg_path) {
        if (config_load(&cfg, cfg_path) != 0) {
            io_printf(2, "[клиент-авто] предупреждение: не удалось прочитать %s, "
                         "используются параметры по умолчанию\n", cfg_path);
        }
    }

    // Инициализация ГСЧ
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 16) ^ (unsigned)dir);

    // Установка обработчиков сигналов
    app_setup_signals(NULL);

    char name[PROTO_NAME_LEN];
    snprintf(name, sizeof(name), "car_d%d", dir);

    ClientConnection conn;
    client_init(&conn, ip, port, ROLE_CAR, dir, name);

    io_printf(1, "[клиент-авто %d] подключение к серверу %s:%s...\n",
              dir, ip, port);

    if (client_connect_with_retry(&conn, 15, 500) != 0) {
        io_printf(2, "[клиент-авто %d] не удалось подключиться к серверу %s:%s\n",
                  dir, ip, port);
        return 1;
    }

    io_printf(1, "[клиент-авто %d] успешно подключён (id #%u, вместимость очередей %d, такт %u мс)\n",
              dir, conn.client_id, conn.cfg.queue_capacity,
              conn.cfg.tick_ms ? conn.cfg.tick_ms : (uint32_t)cfg.tick_ms);

    unsigned long tick = 0;
    unsigned long car_seq = 0;
    unsigned long cars_accepted = 0;
    unsigned long cars_rejected = 0;

    int tick_delay = cfg.tick_ms > 0 ? cfg.tick_ms :
                     (conn.cfg.tick_ms > 0 ? (int)conn.cfg.tick_ms : 50);

    while (!app_signal_received()) {
        if (cfg.ticks_limit > 0 && tick >= (unsigned long)cfg.ticks_limit) {
            io_printf(1, "[клиент-авто %d] достигнут лимит тактов (%ld)\n",
                      dir, cfg.ticks_limit);
            break;
        }
        if (cfg.sim_limit > 0 && cars_accepted >= (unsigned long)cfg.sim_limit) {
            io_printf(1, "[клиент-авто %d] достигнут лимит заявок (%ld)\n",
                      dir, cfg.sim_limit);
            break;
        }

        tick++;

        // Проверка входящих сообщений (SHUTDOWN или обрыв связи)
        ProtoMessage in_msg;
        ProtoPayload in_p;
        int mr = client_recv_message_timeout(&conn, &in_msg, &in_p, 0);
        if (mr == 2) {
            io_printf(1, "[клиент-авто %d] сервер завершил работу (получен SHUTDOWN)\n", dir);
            break;
        } else if (mr < 0) {
            io_printf(2, "[клиент-авто %d] потеряно соединение, попытка переподключения...\n", dir);
            if (client_reconnect_if_needed(&conn) == 0) {
                io_printf(1, "[клиент-авто %d] соединение восстановлено (новый id #%u)\n",
                          dir, conn.client_id);
            } else {
                io_printf(2, "[клиент-авто %d] сервер недоступен, завершение работы\n", dir);
                break;
            }
        }

        // Проверяем вероятность появления машины на этом такте
        int prob = rand() % 100;
        if (prob < cfg.car_arrival_percent) {
            int maneuver = client_choose_maneuver(&conn);
            int emergency = (rand() % 100) < cfg.emergency_percent ? 1 : 0;
            car_seq++;

            int accepted = 0;
            int rc = client_send_car(&conn, maneuver, emergency, &accepted);

            if (rc == 1) {
                io_printf(1, "[клиент-авто %d] сервер завершил работу (получен SHUTDOWN)\n", dir);
                break;
            } else if (rc < 0) {
                io_printf(2, "[клиент-авто %d] потеряно соединение, попытка переподключения...\n", dir);
                if (client_reconnect_if_needed(&conn) == 0) {
                    io_printf(1, "[клиент-авто %d] соединение восстановлено (новый id #%u)\n",
                              dir, conn.client_id);
                } else if (app_signal_received()) {
                    break;
                }
            } else {
                if (accepted) {
                    cars_accepted++;
                    io_printf(1, "[клиент-авто %d] такт %lu: авто #%lu (%s%s) принято сервером в очередь\n",
                              dir, tick, car_seq, model_maneuver_name(maneuver),
                              emergency ? ", ЭКСТРЕННЫЙ" : "");
                } else {
                    cars_rejected++;
                    io_printf(1, "[клиент-авто %d] такт %lu: авто #%lu (%s%s) ОТКАЗ (очередь переполнена)\n",
                              dir, tick, car_seq, model_maneuver_name(maneuver),
                              emergency ? ", ЭКСТРЕННЫЙ" : "");
                }
            }
        }

        model_sleep_ms(tick_delay);
    }

    client_disconnect(&conn);

    io_printf(1, "[клиент-авто %d] завершение: тактов %lu, отправлено %lu, принято %lu, отказов %lu\n",
              dir, tick, car_seq, cars_accepted, cars_rejected);

    return 0;
}
