#ifndef MODEL_H
#define MODEL_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>

#include "config.h"
#include "events.h"

// Состояние одного направления
typedef struct {
    int cars_waiting;
    int cars_emergency;
    int cars_maneuver[MANEUVER_COUNT];
    int peds_waiting;

    unsigned long cars_arrived;
    unsigned long peds_arrived;
    unsigned long cars_served;
    unsigned long peds_served;
    unsigned long wait_ticks;
    unsigned long rejected;
} DirectionSlot;

typedef enum {
    PHASE_GREEN = 0,
    PHASE_TRANSITION = 1
} PhaseState;

typedef struct {
    int current_phase;
    int ticks_in_phase;
    int green_len;
    PhaseState state;
    int transition_left;
    int forced_priority;
} ControllerState;

typedef struct {
    unsigned long ticks;
    unsigned long arrived;
    unsigned long served;
    unsigned long rejected;
    unsigned long wait_ticks;
} Stats;

// Состояние перекрёстка. В ДЗ4 живёт внутри сервера,
// доступ из потоков защищён мьютексом lock.
typedef struct {
    Config cfg;
    DirectionSlot dirs[MAX_DIRECTIONS];
    ControllerState ctrl;
    Stats stats;

    // Флаг остановки. Атомарный, чтобы его можно было выставлять
    // из обработчика сигнала и читать из рабочих потоков.
    atomic_int stop;
    pthread_mutex_t lock;

    // Приёмник событий модели. Устанавливается сервером.
    ModelEventSink event_sink;
    void *event_ctx;
} SharedState;

// Хук, вызываемый контроллером после каждого такта (например, для снимков)
typedef void (*ModelTickHook)(SharedState *st, void *ctx);

// Создание и уничтожение состояния сервера
int  model_init(SharedState *st, const Config *cfg);
void model_destroy(SharedState *st);

// Синхронизация доступа к состоянию из потоков сервера
int  model_lock(SharedState *st);
void model_unlock(SharedState *st);
int  model_stop(const SharedState *st);
void model_request_stop(SharedState *st);

// Передать событие модели установленному приёмнику
void model_emit(SharedState *st, const ModelEvent *ev);

// Слоты направлений
int  model_add_car(SharedState *st, int dir, int emergency, int maneuver);
int  model_add_ped(SharedState *st, int dir);

// Логика фаз
void model_controller_start(SharedState *st);
int  model_controller_step(SharedState *st); /* 1 — модели пора завершаться */
int  model_phase_allows(const SharedState *st, int dir);

// Статистика
void model_print_stats(const SharedState *st);
int  model_save_stats(const SharedState *st, const char *path);

// Потоки сервера
int  controller_run(SharedState *st, const Config *cfg,
                    ModelTickHook tick_hook, void *ctx);
void direction_run(SharedState *st, const Config *cfg, int dir);

void model_sleep_ms(int ms);
const char *model_maneuver_name(int m);
void model_log_phase(const SharedState *st, int phase);
void model_log_queues(SharedState *st);

#endif /* MODEL_H */
