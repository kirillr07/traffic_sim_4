#!/usr/bin/env bash
# run_client_car.sh: запуск клиента-генератора автомобилей

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/app_client_car"

if [ ! -x "$BIN" ]; then
    echo "[run_client_car] бинарник $BIN не найден, выполняю сборку..."
    make -C "$DIR" app_client_car
fi

IP="${1:-127.0.0.1}"
PORT="${2:-45678}"
DIR_NUM="${3:-0}"
CFG="${4:-$DIR/tests/cfg_client_car.txt}"

echo "[run_client_car] Запуск клиента авто: IP=$IP, PORT=$PORT, направление=$DIR_NUM, CFG=$CFG"
exec "$BIN" "$IP" "$PORT" "$DIR_NUM" "$CFG"
