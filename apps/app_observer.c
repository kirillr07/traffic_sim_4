// app_observer.c: сетевой клиент-наблюдатель перекрёстка (ДЗ4)
// Отображает комплексную информацию о состоянии системы в целом:
// фазы светофора, переходные интервалы, очереди автомобилей и пешеходов,
// агрегированную статистику и ленту последних событий.

#include "app.h"
#include "client.h"
#include "io.h"
#include "model.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EVENT_HISTORY_CAP 8

typedef struct {
    ProtoEvent items[EVENT_HISTORY_CAP];
    int count;
    int head;
} EventHistory;

static void history_push(EventHistory *h, const ProtoEvent *ev)
{
    h->items[h->head] = *ev;
    h->head = (h->head + 1) % EVENT_HISTORY_CAP;
    if (h->count < EVENT_HISTORY_CAP)
        h->count++;
}

static const char *maneuver_str(int m)
{
    switch (m) {
    case MANEUVER_STRAIGHT: return "прямо";
    case MANEUVER_RIGHT:    return "направо";
    case MANEUVER_LEFT:     return "налево";
    default:                return "манёвр";
    }
}

static void format_event(const ProtoEvent *ev, char *buf, size_t cap)
{
    switch ((EventKind)ev->kind) {
    case EVENT_CAR_ARRIVED:
        snprintf(buf, cap, "такт %u: прибыло авто #%u на напр. %d (%s%s)",
                 ev->tick, ev->participant_id, ev->direction,
                 maneuver_str(ev->maneuver),
                 ev->emergency ? ", ЭКСТРЕННЫЙ" : "");
        break;
    case EVENT_CAR_QUEUED:
        snprintf(buf, cap, "такт %u: авто #%u в очереди напр. %d (всего в очереди: %d)",
                 ev->tick, ev->participant_id, ev->direction, ev->queue_cars);
        break;
    case EVENT_CAR_REJECTED:
        snprintf(buf, cap, "такт %u: ОТКАЗ авто #%u на напр. %d (очередь переполнена)",
                 ev->tick, ev->participant_id, ev->direction);
        break;
    case EVENT_CAR_PASSED:
        snprintf(buf, cap, "такт %u: проезд авто на напр. %d",
                 ev->tick, ev->direction);
        break;
    case EVENT_PED_REQUEST:
        snprintf(buf, cap, "такт %u: пешеход #%u нажал кнопку вызова на напр. %d",
                 ev->tick, ev->participant_id, ev->direction);
        break;
    case EVENT_PED_QUEUED:
        snprintf(buf, cap, "такт %u: пешеход #%u в очереди напр. %d (всего пешеходов: %d)",
                 ev->tick, ev->participant_id, ev->direction, ev->queue_peds);
        break;
    case EVENT_PED_REJECTED:
        snprintf(buf, cap, "такт %u: ОТКАЗ пешеходу #%u на напр. %d (очередь переполнена)",
                 ev->tick, ev->participant_id, ev->direction);
        break;
    case EVENT_PED_PASSED:
        snprintf(buf, cap, "такт %u: пешеход перешёл улицу на напр. %d",
                 ev->tick, ev->direction);
        break;
    case EVENT_PHASE_START:
        snprintf(buf, cap, "такт %u: начало фазы %d (зелёный сигнал)",
                 ev->tick, ev->phase);
        break;
    case EVENT_PHASE_END:
        snprintf(buf, cap, "такт %u: окончание фазы %d",
                 ev->tick, ev->phase);
        break;
    case EVENT_TRANSITION_START:
        snprintf(buf, cap, "такт %u: переходный интервал безопасности (жёлтый/красный всем)",
                 ev->tick);
        break;
    case EVENT_TRANSITION_END:
        snprintf(buf, cap, "такт %u: конец переходного интервала",
                 ev->tick);
        break;
    case EVENT_EMERGENCY_PRIORITY:
        snprintf(buf, cap, "такт %u: ПРИОРИТЕТ ЭКСТРЕННОМУ ТРАНСПОРТУ!",
                 ev->tick);
        break;
    case EVENT_SERVER_START:
        snprintf(buf, cap, "такт %u: сервер перекрёстка начал работу", ev->tick);
        break;
    case EVENT_SERVER_STOP:
        snprintf(buf, cap, "такт %u: сервер перекрёстка остановлен", ev->tick);
        break;
    default:
        snprintf(buf, cap, "такт %u: событие типа %u на напр. %d",
                 ev->tick, ev->kind, ev->direction);
        break;
    }
}

static void render_screen(const ClientConnection *cli,
                          const ProtoSnapshot *s,
                          const EventHistory *hist)
{
    // Очистка экрана только для интерактивного терминала без запрета переменной окружения
    if (isatty(STDOUT_FILENO) && !getenv("NO_CLEAR")) {
        io_printf(1, "\033[2J\033[H");
    }

    io_printf(1, "==================== [НАБЛЮДАТЕЛЬ: СВОДНЫЙ ЭКРАН] ====================\n");
    io_printf(1, "Такт: %-6u | Клиент #%-4u (%s) | Сервер: %s:%s\n",
              s->tick, cli->client_id, cli->name, cli->ip, cli->port);

    // Статус светофора
    if (s->state == 1 /* PHASE_TRANSITION */) {
        io_printf(1, "Светофор: ПЕРЕХОДНЫЙ ИНТЕРВАЛ (осталось тактов: %d)%s\n",
                  s->transition_left,
                  s->forced_priority ? " [ПРИОРИТЕТ ЭКСТРЕННОМУ]" : "");
    } else {
        io_printf(1, "Светофор: ФАЗА %d [ЗЕЛЁНЫЙ СИГНАЛ] -> разрешены направления:",
                  s->phase);
        int ph = s->phase;
        if (ph >= 0 && ph < cli->cfg.phases_count) {
            for (int d = 0; d < cli->cfg.num_directions; ++d) {
                if (cli->cfg.phases[ph][d])
                    io_printf(1, " %d", d);
            }
        }
        if (s->forced_priority)
            io_printf(1, " [ПРИОРИТЕТ ЭКСТРЕННОМУ]");
        io_printf(1, "\n");
    }

    // Состояние очередей по каждому направлению
    io_printf(1, "----------------------- Очереди по направлениям -----------------------\n");
    io_printf(1, "Напр | Авто (в оч/вместимость) [прямо/право/лево] (экстр) | Пешеходы | Обслужено\n");
    for (int d = 0; d < s->num_directions; ++d) {
        const ProtoDirection *dir = &s->dirs[d];
        io_printf(1, "  %-2d |  %2d/%-2d  [%d/%d/%d]  (экстр: %d)                   |    %2d    | авто: %-3u пеш: %-3u\n",
                  d,
                  dir->cars_waiting, cli->cfg.queue_capacity,
                  dir->cars_maneuver[MANEUVER_STRAIGHT],
                  dir->cars_maneuver[MANEUVER_RIGHT],
                  dir->cars_maneuver[MANEUVER_LEFT],
                  dir->cars_emergency,
                  dir->peds_waiting,
                  dir->cars_served,
                  dir->peds_served);
    }

    // Общая статистика
    double avg_wait = s->served > 0 ? (double)s->wait_ticks / (double)s->served : 0.0;
    io_printf(1, "-------------------------- Общая статистика --------------------------\n");
    io_printf(1, "Прибыло: %-4u | Обслужено: %-4u | Отказов: %-4u | Ср. ожидание: %.2f такта\n",
              s->arrived, s->served, s->rejected, avg_wait);

    // Последние события
    io_printf(1, "-------------------------- Последние события -------------------------\n");
    if (hist->count == 0) {
        io_printf(1, "  (событий пока нет)\n");
    } else {
        int start = (hist->head - hist->count + EVENT_HISTORY_CAP) % EVENT_HISTORY_CAP;
        for (int i = 0; i < hist->count; ++i) {
            int idx = (start + i) % EVENT_HISTORY_CAP;
            char ev_buf[160];
            format_event(&hist->items[idx], ev_buf, sizeof(ev_buf));
            io_printf(1, "  * %s\n", ev_buf);
        }
    }
    io_printf(1, "======================================================================\n\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        io_printf(2, "использование: %s <ip> <port> [имя]\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    const char *port = argv[2];
    const char *name = argc > 3 ? argv[3] : "observer";

    app_setup_signals(NULL);

    ClientConnection conn;
    client_init(&conn, ip, port, ROLE_OBSERVER, -1, name);

    io_printf(1, "[наблюдатель] подключение к серверу %s:%s...\n", ip, port);

    if (client_connect_with_retry(&conn, 15, 500) != 0) {
        io_printf(2, "[наблюдатель] не удалось подключиться к серверу %s:%s\n",
                  ip, port);
        return 1;
    }

    io_printf(1, "[наблюдатель] успешно подключён (id #%u), ожидание снимков модели...\n",
              conn.client_id);

    ProtoSnapshot last_snapshot;
    memset(&last_snapshot, 0, sizeof(last_snapshot));
    int has_snapshot = 0;

    EventHistory hist;
    memset(&hist, 0, sizeof(hist));

    while (!app_signal_received()) {
        ProtoMessage m;
        ProtoPayload p;
        int rc = client_recv_message_timeout(&conn, &m, &p, 1000);

        if (rc == 0) {
            continue;
        }

        if (rc == 2 || m.type == MSG_SHUTDOWN) {
            io_printf(1, "[наблюдатель] получен сигнал завершения от сервера (SHUTDOWN)\n");
            break;
        }

        if (rc < 0) {
            io_printf(2, "[наблюдатель] потеряно соединение, попытка переподключения...\n");
            if (client_reconnect_if_needed(&conn) == 0) {
                io_printf(1, "[наблюдатель] соединение восстановлено (новый id #%u)\n",
                          conn.client_id);
                continue;
            } else {
                if (app_signal_received()) break;
                io_printf(2, "[наблюдатель] сервер недоступен, завершение работы\n");
                break;
            }
        }

        switch ((MsgType)m.type) {
        case MSG_SNAPSHOT:
            last_snapshot = p.snapshot;
            has_snapshot = 1;
            render_screen(&conn, &last_snapshot, &hist);
            break;

        case MSG_EVENT:
            history_push(&hist, &p.event);
            if (has_snapshot)
                render_screen(&conn, &last_snapshot, &hist);
            break;

        case MSG_STATS:
            io_printf(1, "\n[наблюдатель] получена итоговая статистика от сервера:\n");
            io_printf(1, "  тактов: %u, прибыло: %u, обслужено: %u, отказов: %u\n",
                      p.stats.ticks, p.stats.arrived, p.stats.served, p.stats.rejected);
            break;

        case MSG_SHUTDOWN:
            io_printf(1, "[наблюдатель] сервер завершил работу\n");
            goto finish;

        default:
            break;
        }
    }

finish:
    client_disconnect(&conn);
    io_printf(1, "[наблюдатель] работа завершена\n");
    return 0;
}
