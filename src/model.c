#include "model.h"
#include "io.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Инварианты конфигурации

// Направление входит хотя бы в одну фазу
static int direction_in_any_phase(const SharedState *st, int dir)
{
    for (int p = 0; p < st->cfg.phases_count; ++p)
        if (st->cfg.phases[p][dir]) return 1;
    return 0;
}

// Делает матрицу конфликтов симметричной и обнуляет диагональ
static void symmetrize_conflicts(SharedState *st)
{
    for (int i = 0; i < st->cfg.num_directions; ++i) {
        st->cfg.conflicts[i][i] = 0;
        for (int j = i + 1; j < st->cfg.num_directions; ++j) {
            if (st->cfg.conflicts[i][j] || st->cfg.conflicts[j][i])
                st->cfg.conflicts[i][j] = st->cfg.conflicts[j][i] = 1;
        }
    }
}

// Убирает пустые фазы, чтобы контроллер не тратил такты на зеленый без разрешённых направлений
static void compact_phases(SharedState *st)
{
    int write = 0;
    for (int p = 0; p < st->cfg.phases_count; ++p) {
        int nonempty = 0;
        for (int d = 0; d < st->cfg.num_directions; ++d)
            if (st->cfg.phases[p][d]) nonempty = 1;
        if (!nonempty) continue;
        if (write != p) {
            for (int d = 0; d < MAX_DIRECTIONS; ++d)
                st->cfg.phases[write][d] = st->cfg.phases[p][d];
        }
        ++write;
    }
    for (int p = write; p < st->cfg.phases_count; ++p)
        for (int d = 0; d < MAX_DIRECTIONS; ++d)
            st->cfg.phases[p][d] = 0;
    st->cfg.phases_count = write;
}

// Убирает из каждой фазы направления, конфликтующие с уже разрешёнными
static void sanitize_phases(SharedState *st)
{
    int changed = 0;

    symmetrize_conflicts(st);

    for (int p = 0; p < st->cfg.phases_count; ++p) {
        int allowed[MAX_DIRECTIONS];
        memset(allowed, 0, sizeof(allowed));

        for (int d = 0; d < st->cfg.num_directions; ++d) {
            if (!st->cfg.phases[p][d]) continue;

            int conflict = 0;
            for (int e = 0; e < d; ++e) {
                if (allowed[e] && st->cfg.conflicts[e][d]) {
                    conflict = 1;
                    break;
                }
            }

            if (conflict) {
                st->cfg.phases[p][d] = 0;
                changed = 1;
            } else {
                allowed[d] = 1;
            }
        }
    }

    if (changed)
        io_printf(1, "[модель] внимание: конфликтующие направления удалены из фаз\n");
}

// состояние модели

int model_init(SharedState *st, const Config *cfg)
{
    memset(st, 0, sizeof(*st));
    st->cfg = *cfg;

    if (st->cfg.num_directions < 1) st->cfg.num_directions = 1;
    if (st->cfg.num_directions > MAX_DIRECTIONS)
        st->cfg.num_directions = MAX_DIRECTIONS;
    if (st->cfg.phases_count < 1) st->cfg.phases_count = 1;
    if (st->cfg.phases_count > MAX_PHASES)
        st->cfg.phases_count = MAX_PHASES;
    if (st->cfg.num_lanes < 1) st->cfg.num_lanes = 1;
    if (st->cfg.num_lanes > MAX_LANES) st->cfg.num_lanes = MAX_LANES;
    if (st->cfg.queue_capacity < 1) st->cfg.queue_capacity = 1;
    if (st->cfg.phase_min < 1) st->cfg.phase_min = 1;
    if (st->cfg.phase_max < st->cfg.phase_min)
        st->cfg.phase_max = st->cfg.phase_min;
    if (st->cfg.transition < 0) st->cfg.transition = 0;
    if (st->cfg.tick_ms < 0) st->cfg.tick_ms = 0;
    if (st->cfg.car_arrival_percent < 0) st->cfg.car_arrival_percent = 0;
    if (st->cfg.car_arrival_percent > 100) st->cfg.car_arrival_percent = 100;
    if (st->cfg.ped_arrival_percent < 0) st->cfg.ped_arrival_percent = 0;
    if (st->cfg.ped_arrival_percent > 100) st->cfg.ped_arrival_percent = 100;
    if (st->cfg.emergency_percent < 0) st->cfg.emergency_percent = 0;
    if (st->cfg.emergency_percent > 100) st->cfg.emergency_percent = 100;
    sanitize_phases(st);
    compact_phases(st);
    if (st->cfg.phases_count == 0) {
        io_printf(2, "[модель] ошибка: не задано ни одной непустой фазы\n");
        return -1;
    }
    for (int d = 0; d < st->cfg.num_directions; ++d) {
        if (!direction_in_any_phase(st, d)) {
            io_printf(2,
                "[модель] ошибка: направление %d не входит ни в одну фазу\n", d);
            return -1;
        }
    }

    st->ctrl.current_phase = 0;
    st->ctrl.ticks_in_phase = 0;
    st->ctrl.state = PHASE_GREEN;
    st->ctrl.transition_left = 0;
    st->ctrl.forced_priority = 0;
    st->ctrl.green_len = st->cfg.phase_max > 0 ? st->cfg.phase_max : 1;
    atomic_store_explicit(&st->stop, 0, memory_order_relaxed);

    if (pthread_mutex_init(&st->lock, NULL) != 0)
        return -1;

    return 0;
}

void model_destroy(SharedState *st)
{
    pthread_mutex_destroy(&st->lock);
}

// синхронизация внутри сервера: состояние защищено мьютексом
int model_lock(SharedState *st)
{
    return pthread_mutex_lock(&st->lock) == 0 ? 0 : -1;
}

void model_unlock(SharedState *st)
{
    pthread_mutex_unlock(&st->lock);
}

int model_stop(const SharedState *st)
{
    return atomic_load_explicit(&st->stop, memory_order_relaxed) != 0;
}

void model_request_stop(SharedState *st)
{
    atomic_store_explicit(&st->stop, 1, memory_order_relaxed);
}

void model_emit(SharedState *st, const ModelEvent *ev)
{
    if (st->event_sink)
        st->event_sink(ev, st->event_ctx);
}

// направления
int model_add_car(SharedState *st, int dir, int emergency, int maneuver)
{
    if (dir < 0 || dir >= st->cfg.num_directions) return 0;
    if (maneuver < 0 || maneuver >= MANEUVER_COUNT) return 0;
    if (!st->cfg.maneuvers[dir][maneuver]) return 0;
    DirectionSlot *slot = &st->dirs[dir];
    if (st->cfg.queue_capacity > 0 && slot->cars_waiting >= st->cfg.queue_capacity) {
        ++slot->rejected;
        ++st->stats.rejected;
        return 0;
    }
    ++slot->cars_waiting;
    ++slot->cars_maneuver[maneuver];
    if (emergency) ++slot->cars_emergency;
    ++slot->cars_arrived;
    ++st->stats.arrived;
    return 1;
}

int model_add_ped(SharedState *st, int dir)
{
    if (dir < 0 || dir >= st->cfg.num_directions) return 0;
    DirectionSlot *slot = &st->dirs[dir];
    if (st->cfg.queue_capacity > 0 && slot->peds_waiting >= st->cfg.queue_capacity) {
        ++slot->rejected;
        ++st->stats.rejected;
        return 0;
    }
    ++slot->peds_waiting;
    ++slot->peds_arrived;
    ++st->stats.arrived;
    return 1;
}

// логика фаз
static int phase_load(const SharedState *st, int phase)
{
    if (phase < 0 || phase >= st->cfg.phases_count) return 0;

    int load = 0;
    for (int d = 0; d < st->cfg.num_directions; ++d) {
        if (!st->cfg.phases[phase][d]) continue;
        load += st->dirs[d].cars_waiting;
        load += st->dirs[d].peds_waiting;
        load += (int)(st->dirs[d].wait_ticks / 10UL);
    }
    return load;
}

static int emergency_target(const SharedState *st)
{
    if (!st->cfg.emergency_priority) return -1;

    for (int d = 0; d < st->cfg.num_directions; ++d)
        if (st->dirs[d].cars_emergency > 0 && direction_in_any_phase(st, d))
            return d;
    return -1;
}

static int green_length(const SharedState *st)
{
    int lo = st->cfg.phase_min > 0 ? st->cfg.phase_min : 1;
    int hi = st->cfg.phase_max >= lo ? st->cfg.phase_max : lo;

    if (st->cfg.strategy != STRATEGY_ADAPTIVE)
        return hi;

    int len = lo + phase_load(st, st->ctrl.current_phase);
    if (len < lo) len = lo;
    if (len > hi) len = hi;
    return len;
}

static int next_phase(const SharedState *st)
{
    int count = st->cfg.phases_count;
    if (count <= 0) return 0;

    int cur = st->ctrl.current_phase;
    int first = (cur + 1) % count;
    int target = emergency_target(st);
    if (target >= 0) {
        for (int k = 1; k <= count; ++k) {
            int p = (cur + k) % count;
            if (st->cfg.phases[p][target]) return p;
        }
    }

    if (st->cfg.strategy != STRATEGY_ADAPTIVE)
        return first;

    int best = first;
    int best_load = phase_load(st, first);
    for (int k = 2; k < count; ++k) {
        int p = (cur + k) % count;
        int load = phase_load(st, p);
        if (load > best_load) {
            best_load = load;
            best = p;
        }
    }
    return best;
}

static void advance_phase(SharedState *st)
{
    ControllerState *c = &st->ctrl;

    c->current_phase = next_phase(st);
    c->ticks_in_phase = 0;
    c->state = PHASE_GREEN;
    c->transition_left = 0;
    c->green_len = green_length(st);
}

void model_controller_start(SharedState *st)
{
    ControllerState *c = &st->ctrl;

    c->current_phase = 0;
    c->ticks_in_phase = 0;
    c->state = PHASE_GREEN;
    c->transition_left = 0;
    c->forced_priority = 0;
    c->green_len = green_length(st);
}

int model_phase_allows(const SharedState *st, int dir)
{
    if (st->ctrl.state != PHASE_GREEN) return 0;

    int p = st->ctrl.current_phase;
    if (p < 0 || p >= st->cfg.phases_count) return 0;
    if (dir < 0 || dir >= st->cfg.num_directions) return 0;

    return st->cfg.phases[p][dir] != 0;
}

// Забирает из очереди автомобиль с разрешённым манёвром
static void take_car(SharedState *st, DirectionSlot *slot, int emergency)
{
    for (int m = 0; m < MANEUVER_COUNT; ++m) {
        if (slot->cars_maneuver[m] > 0) {
            --slot->cars_maneuver[m];
            break;
        }
    }

    --slot->cars_waiting;
    if (emergency && slot->cars_emergency > 0) --slot->cars_emergency;
    ++slot->cars_served;
    ++st->stats.served;
}

static void serve_direction(SharedState *st, int dir)
{
    DirectionSlot *slot = &st->dirs[dir];
    int lanes = st->cfg.num_lanes > 0 ? st->cfg.num_lanes : 1;
    int take = slot->cars_waiting < lanes ? slot->cars_waiting : lanes;
    if (slot->cars_emergency > 0 && slot->cars_waiting > take)
        ++take;
    int emergency_taken = 0;
    for (int i = 0; i < take; ++i) {
        int is_emergency = slot->cars_emergency > 0 && !emergency_taken;
        take_car(st, slot, is_emergency);
        if (is_emergency) emergency_taken = 1;
    }
    if (slot->peds_waiting > 0) {
        --slot->peds_waiting;
        ++slot->peds_served;
        ++st->stats.served;
    }
}

int model_controller_step(SharedState *st)
{
    ControllerState *c = &st->ctrl;

    ++st->stats.ticks;

    for (int d = 0; d < st->cfg.num_directions; ++d) {
        unsigned long waiting =
            (unsigned long)(st->dirs[d].cars_waiting + st->dirs[d].peds_waiting);
        st->stats.wait_ticks += waiting;
        st->dirs[d].wait_ticks += waiting;
    }

    c->forced_priority = 0;

    if (c->state == PHASE_TRANSITION) {
        if (c->transition_left > 0) --c->transition_left;
        if (c->transition_left <= 0) advance_phase(st);
    } else {
        int target = emergency_target(st);
        int force = 0;
    
        if (target >= 0 && !model_phase_allows(st, target) &&
            c->ticks_in_phase >= st->cfg.phase_min) {
            force = 1;
            c->forced_priority = 1;
        }

        if (!force) {
            for (int d = 0; d < st->cfg.num_directions; ++d)
                if (model_phase_allows(st, d)) serve_direction(st, d);
        }

        ++c->ticks_in_phase;
        if (force || c->ticks_in_phase >= c->green_len) {
            if (st->cfg.transition > 0) {
                c->state = PHASE_TRANSITION;
                c->transition_left = st->cfg.transition;
            } else {
                advance_phase(st);
            }
        }
    }

    int done = 0;
    if (st->cfg.sim_limit > 0 && st->stats.served >= (unsigned long)st->cfg.sim_limit)
        done = 1;
    if (st->cfg.ticks_limit > 0 && st->stats.ticks >= (unsigned long)st->cfg.ticks_limit)
        done = 1;

    if (done) atomic_store_explicit(&st->stop, 1, memory_order_relaxed);
    return done;
}

// вывод событий
const char *model_maneuver_name(int m)
{
    switch (m) {
    case MANEUVER_STRAIGHT: return "прямо";
    case MANEUVER_RIGHT:    return "направо";
    case MANEUVER_LEFT:     return "налево";
    default:                return "?";
    }
}

void model_log_phase(const SharedState *st, int phase)
{
    if (phase < 0 || phase >= st->cfg.phases_count) return;

    char line[512];
    int n = snprintf(line, sizeof(line),
                     "[контроллер] фаза %d разрешает направления:", phase);
    for (int d = 0; d < st->cfg.num_directions && n > 0 &&
                    (size_t)n < sizeof(line); ++d) {
        if (st->cfg.phases[phase][d])
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %d", d);
    }
    if (n > 0 && (size_t)n < sizeof(line))
        snprintf(line + n, sizeof(line) - (size_t)n, "\n");
    io_printf(1, "%s", line);
}

void model_log_queues(SharedState *st)
{
    int counts[MAX_DIRECTIONS];
    int n_dirs = st->cfg.num_directions;

    // Очереди меняются клиентскими потоками — читаем их под мьютексом.
    if (model_lock(st) != 0) return;
    for (int d = 0; d < n_dirs; ++d)
        counts[d] = st->dirs[d].cars_waiting + st->dirs[d].peds_waiting;
    model_unlock(st);

    char line[512];
    int n = snprintf(line, sizeof(line), "[контроллер] очереди:");
    for (int d = 0; d < n_dirs && n > 0 &&
                    (size_t)n < sizeof(line); ++d) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %d:%d", d,
                      counts[d]);
    }
    if (n > 0 && (size_t)n < sizeof(line))
        snprintf(line + n, sizeof(line) - (size_t)n, "\n");

    io_printf(1, "%s", line);
}

// статистика
static double avg_wait(const SharedState *st)
{
    if (st->stats.arrived == 0) return 0.0;
    return (double)st->stats.wait_ticks / (double)st->stats.arrived;
}

static void stats_print(int fd, const SharedState *st)
{
    double avg = avg_wait(st);

    io_printf(fd, "Статистика\n");
    io_printf(fd, "тактов:          %ld\n", (long)st->stats.ticks);
    io_printf(fd, "попыток:         %ld\n",
              (long)(st->stats.arrived + st->stats.rejected));
    io_printf(fd, "принято:         %ld\n", (long)st->stats.arrived);
    io_printf(fd, "обслужено:       %ld\n", (long)st->stats.served);
    io_printf(fd, "отказов/заторов: %ld\n", (long)st->stats.rejected);
    io_printf(fd, "среднее ожидание:%ld.%ld такта\n",
              (long)avg, (long)((avg - (double)(long)avg) * 10.0));

    for (int d = 0; d < st->cfg.num_directions; ++d) {
        const DirectionSlot *s = &st->dirs[d];
        io_printf(fd, "  направление %d: в очереди %ld, прибыло %ld, обслужено %ld, отказов %ld, ожидание %ld\n",
                  d,
                  (long)(s->cars_waiting + s->peds_waiting),
                  (long)(s->cars_arrived + s->peds_arrived),
                  (long)(s->cars_served + s->peds_served),
                  (long)s->rejected,
                  (long)(s->wait_ticks /
                         (s->cars_arrived + s->peds_arrived > 0
                              ? s->cars_arrived + s->peds_arrived : 1)));
    }
}

void model_print_stats(const SharedState *st)
{
    stats_print(1, st);
}

int model_save_stats(const SharedState *st, const char *path)
{
    int fd = io_open_log(path);
    if (fd < 0) return -1;

    stats_print(fd, st);
    io_close(fd);
    return 0;
}

void model_sleep_ms(int ms)
{
    if (ms <= 0) {
        sched_yield();
        return;
    }

    struct timespec ts = {
        ms / 1000,
        (long)(ms % 1000) * 1000000L
    };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}
