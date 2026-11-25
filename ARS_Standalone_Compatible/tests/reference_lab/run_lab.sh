#!/usr/bin/env bash
# Isolated, compiled-in lab profile. Does NOT use INI or runtime config overrides.
set -euo pipefail
LAB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$LAB/../.." && pwd)"
COMPOSE="$LAB/compose.yaml"
BUILD="$ROOT/build-lab"

usage() {
  cat <<'HELP'
Usage: tests/reference_lab/run_lab.sh COMMAND
  up        start isolated PostgreSQL and Redis containers
  build     build server with ARS_LAB_PROFILE=ON and standalone clients
  check     check lab server's compiled-in dependencies and ports
  server    run lab server (no INI and no password environment needed)
  producer  run demo producer (TCP lab port 7093)
  consumer  run interactive consumer (TCP lab port 7093)
  tcp-test  run opt-in live TCP+UDP protocol integration tests
  lifecycle-test  run hot/cold archive/recovery integration on lab DB
  inspect   examine isolated test Redis namespace and SQL counts
  stop      stop lab DB/Redis containers without deleting data
HELP
}

[[ $# -eq 1 ]] || { usage; exit 2; }
case "$1" in
 up)
   docker compose -f "$COMPOSE" up -d --wait ;;
 build)
   cmake -S "$ROOT/server" -B "$BUILD/server" -DARS_LAB_PROFILE=ON -DCMAKE_BUILD_TYPE=Debug
   cmake --build "$BUILD/server" --parallel 4
   cmake -S "$ROOT/producer" -B "$BUILD/producer" -DCMAKE_BUILD_TYPE=Debug
   cmake --build "$BUILD/producer" --parallel 4
   cmake -S "$ROOT/consumer" -B "$BUILD/consumer" -DCMAKE_BUILD_TYPE=Debug
   cmake --build "$BUILD/consumer" --parallel 4 ;;
 check) exec "$BUILD/server/ars_server" --check ;;
 server) exec "$BUILD/server/ars_server" ;;
 producer) exec "$BUILD/producer/ars_producer" --transport tcp --port 7093 --prefix LAB --pause-sec 12 --interval-ms 500 ;;
 consumer) exec "$BUILD/consumer/ars_consumer" --transport tcp --port 7093 ;;
 tcp-test)
   ARS_RUN_INTEGRATION=1 ARS_TEST_HOST=127.0.0.1 \
   ARS_TEST_TCP_PORT=7093 ARS_TEST_UDP_PORT=7094 \
   python3 "$ROOT/tests/udp_tcp_e2e.py" ;;
 lifecycle-test)
   ARS_TEST_HOST=127.0.0.1 ARS_TEST_PORT=7093 \
   python3 "$ROOT/tests/ported/hot_cold_end_to_end.py" ;;
 inspect)
   docker compose -f "$COMPOSE" exec -T redis redis-cli --scan --pattern 'ars_lab_2026:hotv1:*' | head -40
   docker compose -f "$COMPOSE" exec -T postgres psql -U ars_app -d ars_test -c \
      'SELECT id,count(*) AS records FROM ars_records GROUP BY id ORDER BY id LIMIT 40' ;;
 stop) docker compose -f "$COMPOSE" stop ;;
 *) usage; exit 2 ;;
esac
