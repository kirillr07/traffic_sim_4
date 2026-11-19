#ifndef APP_H
#define APP_H

#include "config.h"
#include "model.h"

// Загрузка конфига и применение переопределения стратегии из аргумента
int app_load_config(Config *cfg, const char *path, const char *strategy);

// Обработчики SIGINT/SIGTERM и игнорирование SIGPIPE
void app_setup_signals(SharedState *st);

// Номер последнего полученного сигнала завершения (0 — сигналов не было)
int app_signal_received(void);

// Сохранение статистики в TRAFFIC_LOG или default_log
void app_save_stats(const SharedState *st, const char *default_log);

#endif /* APP_H */
