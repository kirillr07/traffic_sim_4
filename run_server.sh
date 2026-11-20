#!/usr/bin/env bash
# run_server.sh: запуск сервера регулируемого перекрёстка

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/app_server"

if [ ! -x "$BIN" ]; then
    echo "[run_server] бинарник $BIN не найден, выполняю сборку..."
    make -C "$DIR" app_server
fi

IP="${1:-127.0.0.1}"
PORT="${2:-45678}"
CFG="${3:-$DIR/tests/cfg_server.txt}"
STRATEGY="${4:-}"

echo "[run_server] Запуск сервера: IP=$IP, PORT=$PORT, CFG=$CFG ${STRATEGY:+STRATEGY=$STRATEGY}"
exec "$BIN" "$IP" "$PORT" "$CFG" $STRATEGY
