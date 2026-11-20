#!/usr/bin/env bash
# run_observer.sh: запуск клиента-наблюдателя

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/app_observer"

if [ ! -x "$BIN" ]; then
    echo "[run_observer] бинарник $BIN не найден, выполняю сборку..."
    make -C "$DIR" app_observer
fi

IP="${1:-127.0.0.1}"
PORT="${2:-45678}"
NAME="${3:-observer}"

echo "[run_observer] Запуск наблюдателя: IP=$IP, PORT=$PORT, имя=$NAME"
exec "$BIN" "$IP" "$PORT" "$NAME"
