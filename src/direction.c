// direction.c - процесс направления: наблюдает за очередями своего слота

#include "model.h"
#include "io.h"
#include <unistd.h>

void direction_run(SharedState *st, const Config *cfg, int dir)
{
    if (dir < 0 || dir >= st->cfg.num_directions) {
        io_printf(2, "[направление] ошибка: недопустимый номер %d\n", dir);
        return;
    }
    io_printf(1, "[направление %d] процесс запущен (pid %ld)\n",
              dir, (long)getpid());
    int last_cars = -1;
    int last_peds = -1;
    unsigned long last_car_served = 0;
    unsigned long last_ped_served = 0;

    while (!model_stop(st)) {
        if (model_lock(st) != 0) break;
        int cars = st->dirs[dir].cars_waiting;
        int peds = st->dirs[dir].peds_waiting;
        unsigned long car_served = st->dirs[dir].cars_served;
        unsigned long ped_served = st->dirs[dir].peds_served;
        long tick = (long)st->stats.ticks;
        int phase = st->ctrl.current_phase;
        model_unlock(st);

        if (cars != last_cars || peds != last_peds) {
            io_printf(1,
                "[направление %d] такт %ld, фаза %d: очередь авто %d, пешеходов %d\n",
                dir, tick, phase, cars, peds);
            last_cars = cars;
            last_peds = peds;
        }

        if (car_served > last_car_served || ped_served > last_ped_served) {
            io_printf(1,
                "[направление %d] такт %ld, фаза %d: проехало %ld авто, прошло %ld пешеходов\n",
                dir, tick, phase,
                (long)(car_served - last_car_served),
                (long)(ped_served - last_ped_served));
            last_car_served = car_served;
            last_ped_served = ped_served;
        }
        model_sleep_ms(cfg->tick_ms);
    }

    io_printf(1, "[направление %d] процесс завершён\n", dir);
}
