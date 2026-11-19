#include "model.h"
#include "io.h"

#include <string.h>

typedef struct {
    int phase;
    PhaseState state;
    int forced_priority;
    int transition_left;
    unsigned long cars_served[MAX_DIRECTIONS];
    unsigned long peds_served[MAX_DIRECTIONS];
} Snapshot;

static void snapshot(SharedState *st, Snapshot *s)
{
    s->phase = st->ctrl.current_phase;
    s->state = st->ctrl.state;
    s->forced_priority = st->ctrl.forced_priority;
    s->transition_left = st->ctrl.transition_left;

    for (int d = 0; d < st->cfg.num_directions; ++d) {
        s->cars_served[d] = st->dirs[d].cars_served;
        s->peds_served[d] = st->dirs[d].peds_served;
    }
}

static void emit_phase_event(SharedState *st, ModelEventKind kind,
                             int phase, int state, unsigned long tick)
{
    ModelEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.tick = tick;
    ev.kind = kind;
    ev.direction = -1;
    ev.maneuver = -1;
    ev.phase = phase;
    ev.state = state;
    model_emit(st, &ev);
}

static void log_step(SharedState *st, const Snapshot *before,
                     const Snapshot *after)
{
    unsigned long tick = st->stats.ticks;

    if (before->state == PHASE_TRANSITION &&
        after->state == PHASE_GREEN) {
        io_printf(1,
            "[контроллер] такт %ld: конец переходного интервала, начало фазы %d\n",
            (long)tick, after->phase);
        emit_phase_event(st, MODEL_EVENT_TRANSITION_END, after->phase,
                         (int)after->state, tick);
        emit_phase_event(st, MODEL_EVENT_PHASE_START, after->phase,
                         (int)after->state, tick);
        model_log_phase(st, after->phase);
        model_log_queues(st);
    } else if (before->state == PHASE_GREEN &&
               after->state == PHASE_TRANSITION) {
        io_printf(1,
            "[контроллер] такт %ld: конец фазы %d, переходный интервал %d тактов\n",
            (long)tick, before->phase, after->transition_left);
        emit_phase_event(st, MODEL_EVENT_PHASE_END, before->phase,
                         (int)after->state, tick);
        emit_phase_event(st, MODEL_EVENT_TRANSITION_START, after->phase,
                         (int)after->state, tick);
    } else if (before->phase != after->phase) {
        io_printf(1,
            "[контроллер] такт %ld: конец фазы %d, начало фазы %d\n",
            (long)tick, before->phase, after->phase);
        emit_phase_event(st, MODEL_EVENT_PHASE_END, before->phase,
                         (int)after->state, tick);
        emit_phase_event(st, MODEL_EVENT_PHASE_START, after->phase,
                         (int)after->state, tick);
        model_log_phase(st, after->phase);
        model_log_queues(st);
    }

    if (after->forced_priority && !before->forced_priority) {
        io_printf(1,
            "[контроллер] такт %ld: приоритет экстренному транспорту — завершаем фазу %d\n",
            (long)tick, before->phase);
        emit_phase_event(st, MODEL_EVENT_EMERGENCY_PRIORITY, before->phase,
                         (int)after->state, tick);
    }

    unsigned long cars = 0;
    unsigned long peds = 0;
    for (int d = 0; d < st->cfg.num_directions; ++d) {
        if (after->cars_served[d] > before->cars_served[d])
            cars += after->cars_served[d] - before->cars_served[d];
        if (after->peds_served[d] > before->peds_served[d])
            peds += after->peds_served[d] - before->peds_served[d];
    }

    if (cars > 0 || peds > 0) {
        io_printf(1,
            "[контроллер] такт %ld, фаза %d: проезд %ld авто, переход %ld пешеходов\n",
            (long)st->stats.ticks, after->phase, (long)cars, (long)peds);
    }
}

int controller_run(SharedState *st, const Config *cfg,
                   ModelTickHook tick_hook, void *ctx)
{
    Snapshot before;
    Snapshot after;
    if (model_lock(st) != 0) return 1;
    model_controller_start(st);
    snapshot(st, &before);
    model_unlock(st);

    io_printf(1, "[контроллер] старт: фаз %ld, направлений %d, стратегия %s\n",
              (long)cfg->phases_count, cfg->num_directions,
              cfg->strategy == STRATEGY_ADAPTIVE ? "adaptive" : "fixed");
    model_log_phase(st, before.phase);
    model_log_queues(st);
    emit_phase_event(st, MODEL_EVENT_PHASE_START, before.phase,
                     (int)before.state, st->stats.ticks);

    if (tick_hook) tick_hook(st, ctx);

    while (!model_stop(st)) {
        if (model_lock(st) != 0) break;
        snapshot(st, &before);
        int done = model_controller_step(st);
        snapshot(st, &after);
        model_unlock(st);
        log_step(st, &before, &after);
        if (tick_hook) tick_hook(st, ctx);
        if (done) break;
        model_sleep_ms(cfg->tick_ms);
    }

    io_printf(1, "[контроллер] остановлен на такте %ld\n",
              (long)st->stats.ticks);
    return 0;
}
