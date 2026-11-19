// config.h - параметры модели перекрёстка.

#ifndef CONFIG_H
#define CONFIG_H
#define MAX_DIRECTIONS 8
#define MAX_LANES      4
#define MAX_PHASES     8
#define MAX_PATH_LEN   256
#define MANEUVER_COUNT 3
#define MANEUVER_STRAIGHT 0
#define MANEUVER_RIGHT    1
#define MANEUVER_LEFT     2

// Стратегия управления контроллера
typedef enum {
    STRATEGY_FIXED = 0,   // фиксированный цикл
    STRATEGY_ADAPTIVE = 1 // по длинам очередей и времени ожидания
} ControlStrategy;

// Конфигурация модели. Значения по умолчанию задаются в config_defaults(), затем переопределяются из файла
typedef struct {
    int num_directions;   // число направлений
    int num_lanes;        // полос на направление
    int queue_capacity;   // вместимость одной очереди
    int car_arrival_percent;   // вероятность появления машины за такт, 0-100%
    int ped_arrival_percent;   // вероятность появления пешехода за такт, 0-100%
    int emergency_percent;     // доля экстренных среди машин, 0-100%
    int emergency_priority;    // 1 - приоритет экстренному после текущей фазы
    int phase_min;        // минимальная длительность фазы
    int phase_max;        // максимальная длительность фазы
    int transition;       // переходный интервал безопасности

    // Стратегия
    ControlStrategy strategy;

    // Разрешённые манёвры: maneuvers[d][m] == 1 для направления d (0 — прямо, 1 — направо, 2 — налево)
    int maneuvers[MAX_DIRECTIONS][MANEUVER_COUNT];

    // Ограничения завершения (0 = без ограничения)
    long sim_limit;       // макс. число обслуженных участников
    long ticks_limit;     // макс. число тактов наблюдения

    // Визуализация
    int tick_ms;          // задержка на такт, миллисекунды

    // Матрица конфликтов: conflicts[i][j] == 1, если направления i и j несовместимы
    // Диагональ = 0 т.к направление не конфликтует само с собой
    int conflicts[MAX_DIRECTIONS][MAX_DIRECTIONS];

    // Фазы: каждая фаза - множество разрешённых направлений
    // phases[p][d] == 1, если в фазе p разрешено направление d
    // phases_count - сколько фаз реально задано
    int phases[MAX_PHASES][MAX_DIRECTIONS];
    int phases_count;
} Config;

// Загружает конфигурацию из файла. Сначала выставляет значения по умолчанию,
// потом переопределяет их тем, что найдено в файле, возвращает 0 при успехе, -1 при ошибке
int config_load(Config *cfg, const char *path);

// Печатает конфигурацию для отладки и лога
void config_print(const Config *cfg);

#endif //CONFIG_H
