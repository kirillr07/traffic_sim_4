// app.c: общий код приложений: конфиг, сигналы, лог

#include "app.h"
#include "io.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

static SharedState *g_signal_state = NULL;

static void handle_signal(int sig)
{
    (void)sig;
    if (g_signal_state) g_signal_state->stop = 1;
}

void app_setup_signals(SharedState *st)
{
    g_signal_state = st;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, NULL);
}

int app_load_config(Config *cfg, const char *path, const char *strategy)
{
    if (config_load(cfg, path) != 0) {
        io_printf(2, "ошибка: не удалось загрузить конфигурацию %s\n", path);
        return -1;
    }

    if (strategy) {
        if (strcmp(strategy, "adaptive") == 0)
            cfg->strategy = STRATEGY_ADAPTIVE;
        else if (strcmp(strategy, "fixed") == 0)
            cfg->strategy = STRATEGY_FIXED;
    }
    return 0;
}

void app_save_stats(const SharedState *st, const char *default_log)
{
    const char *path = getenv("TRAFFIC_LOG");
    if (!path || !*path) path = default_log;

    if (model_save_stats(st, path) == 0)
        io_printf(1, "лог статистики сохранён: %s\n", path);
}
