set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

LOG_DIR="$DIR/tests"
mkdir -p "$LOG_DIR"

TOTAL_TESTS=0
PASSED_TESTS=0

log_header() {
    echo -e "\n${BLUE}======================================================================${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}======================================================================${NC}"
}

log_pass() {
    echo -e "  ${GREEN}[OK] $1${NC}"
    ((PASSED_TESTS++)) || true
}

log_fail() {
    echo -e "  ${RED}[FAIL] $1${NC}"
}

RUNNING_PIDS=""

kill_pids() {
    for pid in $RUNNING_PIDS; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
    RUNNING_PIDS=""
}

trap kill_pids EXIT INT TERM

wait_for_port() {
    local ip="$1"
    local port="$2"
    local tries=30
    while [ $tries -gt 0 ]; do
        if (echo > /dev/tcp/"$ip"/"$port") 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        ((tries--)) || true
    done
    return 1
}

# Сборка проекта
((TOTAL_TESTS++)) || true
log_header "0. Сборка проекта (Makefile)"
if make all test_protocol; then
    log_pass "Все бинарники и тестовые утилиты успешно собраны с флагами -Wall -Wextra -Wpedantic"
else
    log_fail "Ошибка сборки make all test_protocol"
    exit 1
fi

# Тест протокола
((TOTAL_TESTS++)) || true
log_header "1. Модульный тест протокола (test_protocol)"
if ./test_protocol; then
    log_pass "Протокол: сериализация, десериализация и round-trip пройдены"
else
    log_fail "Ошибка в тестах протокола"
fi

#Сервер + Клиенты авто и пешеходов
((TOTAL_TESTS++)) || true
log_header "Независимые клиенты авто и пешеходов"
PORT=45801
SRV_LOG="$LOG_DIR/out_srv_4_5.log"
CAR0_LOG="$LOG_DIR/out_car0_4_5.log"
CAR1_LOG="$LOG_DIR/out_car1_4_5.log"
PED0_LOG="$LOG_DIR/out_ped0_4_5.log"

./app_server 127.0.0.1 $PORT tests/cfg_server.txt > "$SRV_LOG" 2>&1 &
SRV_PID=$!
RUNNING_PIDS="$SRV_PID"

if wait_for_port 127.0.0.1 $PORT; then
    ./app_client_car 127.0.0.1 $PORT 0 tests/cfg_client_car.txt > "$CAR0_LOG" 2>&1 &
    CAR0_PID=$!
    ./app_client_car 127.0.0.1 $PORT 1 tests/cfg_client_car.txt > "$CAR1_LOG" 2>&1 &
    CAR1_PID=$!
    ./app_client_ped 127.0.0.1 $PORT 0 tests/cfg_client_ped.txt > "$PED0_LOG" 2>&1 &
    PED0_PID=$!
    RUNNING_PIDS="$RUNNING_PIDS $CAR0_PID $CAR1_PID $PED0_PID"

    wait "$SRV_PID"
    wait "$CAR0_PID"
    wait "$CAR1_PID"
    wait "$PED0_PID"
    RUNNING_PIDS=""

    # Проверка, что клиенты авто и пешеходов получали ответы от сервера
    if grep -q "принято сервером в очередь" "$CAR0_LOG" && \
       grep -q "принято сервером в очередь" "$CAR1_LOG" && \
       grep -q "принят сервером в очередь" "$PED0_LOG"; then
        log_pass "Клиенты успешно обмениваются сообщениями и отображают локальный вывод"
    else
        log_fail "Не найдены подтверждения постановки в очередь в логах клиентов"
    fi
else
    log_fail "Сервер не открыл порт $PORT вовремя"
fi

# Комплексный наблюдатель со сводным экраном
((TOTAL_TESTS++)) || true
log_header "Клиент-наблюдатель перекрёстка"
PORT=45802
SRV_LOG="$LOG_DIR/out_srv_6_7.log"
OBS_LOG="$LOG_DIR/out_obs_6_7.log"

./app_server 127.0.0.1 $PORT tests/cfg_server.txt > "$SRV_LOG" 2>&1 &
SRV_PID=$!
RUNNING_PIDS="$SRV_PID"

if wait_for_port 127.0.0.1 $PORT; then
    export NO_CLEAR=1
    ./app_observer 127.0.0.1 $PORT obs_main > "$OBS_LOG" 2>&1 &
    OBS_PID=$!
    ./app_client_car 127.0.0.1 $PORT 0 tests/cfg_client_car.txt > /dev/null 2>&1 &
    CAR_PID=$!
    ./app_client_ped 127.0.0.1 $PORT 1 tests/cfg_client_ped.txt > /dev/null 2>&1 &
    PED_PID=$!
    RUNNING_PIDS="$RUNNING_PIDS $OBS_PID $CAR_PID $PED_PID"

    wait "$SRV_PID"
    wait "$OBS_PID"
    wait "$CAR_PID"
    wait "$PED_PID"
    RUNNING_PIDS=""

    if grep -q "НАБЛЮДАТЕЛЬ: СВОДНЫЙ ЭКРАН" "$OBS_LOG" && \
       grep -q "Светофор: ФАЗА" "$OBS_LOG" && \
       grep -q "Очереди по направлениям" "$OBS_LOG" && \
       grep -q "Общая статистика" "$OBS_LOG" && \
       grep -q "Последние события" "$OBS_LOG"; then
        log_pass "Наблюдатель отображает комплексную информацию: фазы, очереди, статистику и события"
    else
        log_fail "В выводе наблюдателя отсутствуют необходимые разделы сводного экрана"
    fi
else
    log_fail "Сервер не открыл порт $PORT"
fi

# Требования на 8 баллов: Множественные наблюдатели и динамическое отключение/подключение
((TOTAL_TESTS++)) || true
log_header "Множественные наблюдатели (убийство одного, подключение на лету)"
PORT=45803
SRV_LOG="$LOG_DIR/out_srv_8.log"
OBS1_LOG="$LOG_DIR/out_obs1_8.log"
OBS2_LOG="$LOG_DIR/out_obs2_8.log"
OBS3_LOG="$LOG_DIR/out_obs3_8.log"
OBS4_LOG="$LOG_DIR/out_obs4_8.log"

./app_server 127.0.0.1 $PORT tests/cfg_server.txt > "$SRV_LOG" 2>&1 &
SRV_PID=$!
RUNNING_PIDS="$SRV_PID"

if wait_for_port 127.0.0.1 $PORT; then
    export NO_CLEAR=1
    ./app_observer 127.0.0.1 $PORT obs1 > "$OBS1_LOG" 2>&1 &
    OBS1_PID=$!
    ./app_observer 127.0.0.1 $PORT obs2 > "$OBS2_LOG" 2>&1 &
    OBS2_PID=$!
    ./app_observer 127.0.0.1 $PORT obs3 > "$OBS3_LOG" 2>&1 &
    OBS3_PID=$!
    ./app_client_car 127.0.0.1 $PORT 0 tests/cfg_client_car.txt > /dev/null 2>&1 &
    CAR_PID=$!
    RUNNING_PIDS="$RUNNING_PIDS $OBS1_PID $OBS2_PID $OBS3_PID $CAR_PID"

    sleep 0.8
    # Убиваем наблюдателя 2 аварийным сигналом SIGKILL
    kill -KILL "$OBS2_PID" 2>/dev/null || true
    wait "$OBS2_PID" 2>/dev/null || true

    sleep 0.4
    # Проверяем, что сервер и остальные наблюдатели работают
    if kill -0 "$SRV_PID" 2>/dev/null && \
       kill -0 "$OBS1_PID" 2>/dev/null && \
       kill -0 "$OBS3_PID" 2>/dev/null; then
        # Подключаем нового наблюдателя 4 на лету
        ./app_observer 127.0.0.1 $PORT obs4 > "$OBS4_LOG" 2>&1 &
        OBS4_PID=$!
        RUNNING_PIDS="$RUNNING_PIDS $OBS4_PID"

        wait "$SRV_PID"
        wait "$OBS1_PID"
        wait "$OBS3_PID"
        wait "$OBS4_PID"
        wait "$CAR_PID"
        RUNNING_PIDS=""

        if grep -q "НАБЛЮДАТЕЛЬ: СВОДНЫЙ ЭКРАН" "$OBS4_LOG"; then
            log_pass "Множественные наблюдатели стабильны: отключение одного не ломает систему, новый подключён на лету"
        else
            log_fail "Новый наблюдатель obs4 не получил снимки перекрёстка"
        fi
    else
        log_fail "Сервер или оставшиеся наблюдатели упали после убийства obs2"
    fi
else
    log_fail "Сервер не открыл порт $PORT"
fi

# Завершение по SIGINT серверу и корректный выход клиентов
((TOTAL_TESTS++)) || true
log_header "SIGINT серверу -> рассылка SHUTDOWN -> клиенты выходят с кодом 0"
PORT=45804
SRV_LOG="$LOG_DIR/out_srv_sig.log"
CAR_LOG="$LOG_DIR/out_car_sig.log"
PED_LOG="$LOG_DIR/out_ped_sig.log"
OBS_LOG="$LOG_DIR/out_obs_sig.log"

./app_server 127.0.0.1 $PORT tests/cfg_server.txt > "$SRV_LOG" 2>&1 &
SRV_PID=$!
RUNNING_PIDS="$SRV_PID"

if wait_for_port 127.0.0.1 $PORT; then
    ./app_client_car 127.0.0.1 $PORT 0 tests/cfg_client_car.txt > "$CAR_LOG" 2>&1 &
    CAR_PID=$!
    ./app_client_ped 127.0.0.1 $PORT 0 tests/cfg_client_ped.txt > "$PED_LOG" 2>&1 &
    PED_PID=$!
    export NO_CLEAR=1
    ./app_observer 127.0.0.1 $PORT obs_sig > "$OBS_LOG" 2>&1 &
    OBS_PID=$!
    RUNNING_PIDS="$RUNNING_PIDS $CAR_PID $PED_PID $OBS_PID"
    sleep 1.0
    kill -INT "$SRV_PID"
    wait "$SRV_PID"
    SRV_RC=$?
    wait "$CAR_PID"
    CAR_RC=$?
    wait "$PED_PID"
    PED_RC=$?
    wait "$OBS_PID"
    OBS_RC=$?
    RUNNING_PIDS=""

    if [ $SRV_RC -eq 0 ] && [ $CAR_RC -eq 0 ] && [ $PED_RC -eq 0 ] && [ $OBS_RC -eq 0 ]; then
        log_pass "Все процессы (сервер, авто, пешеход, наблюдатель) завершились с кодом 0 по SIGINT"
    else
        log_fail "Ненулевой код возврата: srv=$SRV_RC, car=$CAR_RC, ped=$PED_RC, obs=$OBS_RC"
    fi
else
    log_fail "Сервер не открыл порт $PORT"
fi

# Устойчивость сервера при отключении и перезапуске клиентов
((TOTAL_TESTS++)) || true
log_header "Отключение клиента и его повторный запуск без сбоя сервера"
PORT=45805
SRV_LOG="$LOG_DIR/out_srv_reclient.log"
CAR1_LOG="$LOG_DIR/out_car_rec1.log"
CAR2_LOG="$LOG_DIR/out_car_rec2.log"

./app_server 127.0.0.1 $PORT tests/cfg_server.txt > "$SRV_LOG" 2>&1 &
SRV_PID=$!
RUNNING_PIDS="$SRV_PID"

if wait_for_port 127.0.0.1 $PORT; then
    ./app_client_car 127.0.0.1 $PORT 2 tests/cfg_client_car.txt > "$CAR1_LOG" 2>&1 &
    CAR1_PID=$!
    RUNNING_PIDS="$RUNNING_PIDS $CAR1_PID"

    sleep 0.6
    kill -TERM "$CAR1_PID"
    wait "$CAR1_PID" 2>/dev/null || true

    sleep 0.3
    if kill -0 "$SRV_PID" 2>/dev/null; then
        ./app_client_car 127.0.0.1 $PORT 2 tests/cfg_client_car.txt > "$CAR2_LOG" 2>&1 &
        CAR2_PID=$!
        RUNNING_PIDS="$RUNNING_PIDS $CAR2_PID"

        sleep 0.8
        kill -INT "$SRV_PID"
        wait "$SRV_PID"
        wait "$CAR2_PID"
        RUNNING_PIDS=""

        if grep -q "принято сервером в очередь" "$CAR2_LOG"; then
            log_pass "Клиент был отключён, сервер сохранил работоспособность, перезапущенный клиент продолжил обмен"
        else
            log_fail "Перезапущенный клиент не смог отправить машины серверу"
        fi
    else
        log_fail "Сервер упал после остановки клиента"
    fi
else
    log_fail "Сервер не открыл порт $PORT"
fi

# Клиент ждёт запуск сервера и переподключается
((TOTAL_TESTS++)) || true
log_header "Клиент с повторными попытками ожидает старт сервера"
PORT=45806
SRV_LOG="$LOG_DIR/out_srv_delayed.log"
CAR_LOG="$LOG_DIR/out_car_delayed.log"

# Запускаем клиента ДО запуска сервера
./app_client_car 127.0.0.1 $PORT 0 tests/cfg_client_car.txt > "$CAR_LOG" 2>&1 &
CAR_PID=$!
RUNNING_PIDS="$CAR_PID"

sleep 0.6
# Сервер запускается с задержкой
./app_server 127.0.0.1 $PORT tests/cfg_server.txt > "$SRV_LOG" 2>&1 &
SRV_PID=$!
RUNNING_PIDS="$RUNNING_PIDS $SRV_PID"

sleep 1.0
kill -INT "$SRV_PID"
wait "$SRV_PID"
wait "$CAR_PID"
RUNNING_PIDS=""

if grep -q "успешно подключён" "$CAR_LOG" && grep -q "принято сервером в очередь" "$CAR_LOG"; then
    log_pass "Клиент устойчив к временной недоступности сервера: дождался запуска и начал обмен"
else
    log_fail "Клиент не подключился к запущенному позже серверу"
fi

# Итог
log_header "ИТОГИ ТЕСТИРОВАНИЯ"
echo -e "Пройдено тестов: ${GREEN}$PASSED_TESTS${NC} из ${BLUE}$TOTAL_TESTS${NC}"

if [ "$PASSED_TESTS" -eq "$TOTAL_TESTS" ]; then
    echo -e "${GREEN}Все тестовые сценарии были успешно пройдены${NC}\n"
    exit 0
else
    echo -e "${RED}Некоторые тесты завершились с ошибкой, см. логи в $LOG_DIR${NC}\n"
    exit 1
fi
