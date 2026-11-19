// config.c - загрузка и печать параметров модели

#include "config.h"
#include "io.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// значения по умолчанию
static void config_defaults(Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->num_directions   = 4;
    cfg->num_lanes        = 2;
    cfg->queue_capacity   = 10;

    cfg->car_arrival_percent = 30;
    cfg->ped_arrival_percent = 10;
    cfg->emergency_percent   = 1;
    cfg->emergency_priority  = 1;

    cfg->phase_min        = 3;
    cfg->phase_max        = 10;
    cfg->transition       = 2;

    cfg->strategy         = STRATEGY_FIXED;

    cfg->sim_limit        = 0;
    cfg->ticks_limit      = 200;
    cfg->tick_ms          = 200;

    // По умолчанию направления 0 и 2 совместимы, 1 и 3 совместимы, остальные пары конфликтуют
    for (int i = 0; i < MAX_DIRECTIONS; ++i)
        for (int j = 0; j < MAX_DIRECTIONS; ++j)
            cfg->conflicts[i][j] = ((i ^ j) & 1) ? 1 : 0;
    for (int i = 0; i < MAX_DIRECTIONS; ++i)
        cfg->conflicts[i][i] = 0;

    // По умолчанию две фазы: 0,1 и 2,3 (для 4 направлений)
    cfg->phases_count = 2;
    cfg->phases[0][0] = 1;
    cfg->phases[0][1] = 1;
    cfg->phases[1][2] = 1;
    cfg->phases[1][3] = 1;

    // По умолчанию разрешены все манёвры во всех направлениях
    for (int d = 0; d < MAX_DIRECTIONS; ++d)
        for (int m = 0; m < MANEUVER_COUNT; ++m)
            cfg->maneuvers[d][m] = 1;
}

// Разбор текстового файла

// Обрезает пробелы слева и справа, возвращает указатель на начало
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) ++s;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) --end;
    *(end + 1) = '\0';
    return s;
}

// Разбирает "conflicts = ..."
static int parse_conflicts(Config *cfg, const char *value)
{
    char buf[MAX_PATH_LEN * 4];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int i = 0;
    char *save_row = NULL;
    char *row = strtok_r(buf, "|", &save_row);
    while (row && i < cfg->num_directions) {
        int j = 0;
        char *save_col = NULL;
        char *tok = strtok_r(row, " \t", &save_col);
        while (tok && j < cfg->num_directions) {
            cfg->conflicts[i][j] = atoi(tok);
            ++j;
            tok = strtok_r(NULL, " \t", &save_col);
        }
        ++i;
        row = strtok_r(NULL, "|", &save_row);
    }
    return 0;
}

// Разбирает "phases = 0,1 | 2,3"
static int parse_phases(Config *cfg, const char *value)
{
    char buf[MAX_PATH_LEN * 4];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    memset(cfg->phases, 0, sizeof(cfg->phases));
    cfg->phases_count = 0;

    char *save_phase = NULL;
    char *phase = strtok_r(buf, "|", &save_phase);
    while (phase && cfg->phases_count < MAX_PHASES) {
        char *save_dir = NULL;
        char *tok = strtok_r(phase, ", \t", &save_dir);
        while (tok) {
            int d = atoi(tok);
            if (d >= 0 && d < cfg->num_directions)
                cfg->phases[cfg->phases_count][d] = 1;
            tok = strtok_r(NULL, ", \t", &save_dir);
        }
        ++cfg->phases_count;
        phase = strtok_r(NULL, "|", &save_phase);
    }
    return 0;
}

// Разбирает "maneuvers = straight,right | left | ..."
static int maneuver_index(const char *tok)
{
    if (strcmp(tok, "straight") == 0 || strcmp(tok, "s") == 0 ||
        strcmp(tok, "0") == 0)
        return MANEUVER_STRAIGHT;
    if (strcmp(tok, "right") == 0 || strcmp(tok, "r") == 0 ||
        strcmp(tok, "1") == 0)
        return MANEUVER_RIGHT;
    if (strcmp(tok, "left") == 0 || strcmp(tok, "l") == 0 ||
        strcmp(tok, "2") == 0)
        return MANEUVER_LEFT;
    return -1;
}

static int parse_maneuvers(Config *cfg, const char *value)
{
    char buf[MAX_PATH_LEN * 4];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int i = 0;
    char *save_row = NULL;
    char *row = strtok_r(buf, "|", &save_row);
    while (row && i < cfg->num_directions) {
        for (int m = 0; m < MANEUVER_COUNT; ++m)
            cfg->maneuvers[i][m] = 0;

        char *save_tok = NULL;
        char *tok = strtok_r(row, ", \t", &save_tok);
        while (tok) {
            if (strcmp(tok, "all") == 0) {
                for (int m = 0; m < MANEUVER_COUNT; ++m)
                    cfg->maneuvers[i][m] = 1;
            } else {
                int m = maneuver_index(tok);
                if (m >= 0) cfg->maneuvers[i][m] = 1;
            }
            tok = strtok_r(NULL, ", \t", &save_tok);
        }
        ++i;
        row = strtok_r(NULL, "|", &save_row);
    }
    return 0;
}

// Парсер файла
int config_load(Config *cfg, const char *path)
{
    config_defaults(cfg);

    int fd = io_open_read(path);
    if (fd < 0) return -1;

    char line[MAX_PATH_LEN * 4];
    ssize_t n;
    while ((n = io_read_line(fd, line, sizeof(line))) >= 0) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key   = trim(p);
        char *value = trim(eq + 1);

        char *hash = strchr(value, '#');
        if (hash) { *hash = '\0'; value = trim(value); }

        if      (strcmp(key, "num_directions")   == 0) cfg->num_directions   = atoi(value);
        else if (strcmp(key, "num_lanes")        == 0) cfg->num_lanes        = atoi(value);
        else if (strcmp(key, "queue_capacity")   == 0) cfg->queue_capacity   = atoi(value);
        else if (strcmp(key, "car_arrival_percent") == 0) cfg->car_arrival_percent = atoi(value);
        else if (strcmp(key, "ped_arrival_percent") == 0) cfg->ped_arrival_percent = atoi(value);
        else if (strcmp(key, "emergency_percent")   == 0) cfg->emergency_percent   = atoi(value);
        else if (strcmp(key, "emergency_priority")  == 0) cfg->emergency_priority  = atoi(value);
        else if (strcmp(key, "phase_min")        == 0) cfg->phase_min        = atoi(value);
        else if (strcmp(key, "phase_max")        == 0) cfg->phase_max        = atoi(value);
        else if (strcmp(key, "transition")       == 0) cfg->transition       = atoi(value);
        else if (strcmp(key, "sim_limit")        == 0) cfg->sim_limit        = atol(value);
        else if (strcmp(key, "ticks_limit")      == 0) cfg->ticks_limit      = atol(value);
        else if (strcmp(key, "tick_ms")          == 0) cfg->tick_ms          = atoi(value);
        else if (strcmp(key, "strategy")         == 0) {
            if (strcmp(value, "adaptive") == 0) cfg->strategy = STRATEGY_ADAPTIVE;
            else                                 cfg->strategy = STRATEGY_FIXED;
        }
        else if (strcmp(key, "conflicts") == 0) parse_conflicts(cfg, value);
        else if (strcmp(key, "phases")    == 0) parse_phases(cfg, value);
        else if (strcmp(key, "maneuvers") == 0) parse_maneuvers(cfg, value);
    }

    io_close(fd);
    return 0;
}

// Печать
void config_print(const Config *cfg)
{
    io_printf(1, "Конфигурация\n");
    io_printf(1, "направлений:     %d\n", cfg->num_directions);
    io_printf(1, "полос:           %d\n", cfg->num_lanes);
    io_printf(1, "вместимость:     %d\n", cfg->queue_capacity);
    io_printf(1, "P(машина):       %d%%\n", cfg->car_arrival_percent);
    io_printf(1, "P(пешеход):      %d%%\n", cfg->ped_arrival_percent);
    io_printf(1, "P(экстренная):   %d%%\n", cfg->emergency_percent);
    io_printf(1, "приоритет экстр.: %d\n", cfg->emergency_priority);
    io_printf(1, "фаза min/max:    %d / %d\n", cfg->phase_min, cfg->phase_max);
    io_printf(1, "переход:         %d\n", cfg->transition);
    io_printf(1, "стратегия:       %s\n", cfg->strategy == STRATEGY_ADAPTIVE ? "adaptive" : "fixed");
    io_printf(1, "sim_limit:       %ld\n", cfg->sim_limit);
    io_printf(1, "ticks_limit:     %ld\n", cfg->ticks_limit);
    io_printf(1, "tick_ms:         %d\n", cfg->tick_ms);
    io_printf(1, "фазы:\n");
    for (int p = 0; p < cfg->phases_count; ++p) {
        io_printf(1, "  фаза %d: ", p);
        for (int d = 0; d < cfg->num_directions; ++d)
            if (cfg->phases[p][d]) io_printf(1, "%d ", d);
        io_printf(1, "\n");
    }
    io_printf(1, "манёвры:\n");
    static const char *names[MANEUVER_COUNT] = { "прямо", "направо", "налево" };
    for (int d = 0; d < cfg->num_directions; ++d) {
        io_printf(1, "  направление %d: ", d);
        for (int m = 0; m < MANEUVER_COUNT; ++m)
            if (cfg->maneuvers[d][m]) io_printf(1, "%s ", names[m]);
        io_printf(1, "\n");
    }
}
