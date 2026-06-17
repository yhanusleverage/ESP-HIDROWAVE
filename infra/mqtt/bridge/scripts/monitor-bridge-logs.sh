#!/usr/bin/env bash
# Acompanha logs do serviço bridge na VM AWS (journalctl).
# Uso na Lightsail:
#   sudo bash scripts/monitor-bridge-logs.sh
# Ou via SSH:
#   ssh -i key.pem ubuntu@99.79.36.220 'sudo journalctl -u hidrowave-bridge -f --no-pager' \
#     | grep -E --line-buffered 'INSERT|PATCH|Rejected|Throttled|error|ph_|ec_|telemetry'

set -euo pipefail

UNIT="${BRIDGE_UNIT:-hidrowave-bridge}"
PATTERN="${MONITOR_LOG_PATTERN:-INSERT|PATCH|Rejected|Throttled|error|ph_|ec_|telemetry|Subscribed}"

echo ">> journalctl -u ${UNIT} -f (filter: ${PATTERN})"
echo ">> Ctrl+C para sair"
echo "────────────────────────────────────────────────────────────────────────"

journalctl -u "${UNIT}" -f --no-pager 2>/dev/null \
  | grep -E --line-buffered "${PATTERN}" \
  || journalctl -u "${UNIT}" -f --no-pager
