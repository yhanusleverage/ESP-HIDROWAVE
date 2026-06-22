#!/usr/bin/env bash
# Deploy bridge EC-only para /opt/hidrowave-bridge (Lightsail/Ubuntu)
#
# Uso (na máquina local, com chave SSH):
#   BRIDGE_HOST=ubuntu@99.79.36.220 BRIDGE_KEY=~/sua-chave.pem ./scripts/deploy-to-server.sh
#
# Pré-requisitos no servidor:
#   - /opt/hidrowave-bridge com node_modules instalado
#   - .env com SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, MQTT_HOST (sem colar no shell)
#   - ADD_HYDRO_EC_COLUMN.sql executado no Supabase SQL Editor

set -euo pipefail

HOST="${BRIDGE_HOST:-ubuntu@99.79.36.220}"
KEY="${BRIDGE_KEY:-}"
REMOTE_DIR="/opt/hidrowave-bridge"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SSH_OPTS=()
if [[ -n "$KEY" ]]; then
  SSH_OPTS+=(-i "$KEY")
fi

echo "==> Backup index.js remoto"
ssh "${SSH_OPTS[@]}" "$HOST" "sudo cp $REMOTE_DIR/index.js $REMOTE_DIR/index.js.bak.\$(date +%Y%m%d%H%M) || true"

echo "==> Copiar index.js + scripts"
scp "${SSH_OPTS[@]}" "$BRIDGE_DIR/index.js" "$HOST:/tmp/hidrowave-index.js"
scp "${SSH_OPTS[@]}" -r "$BRIDGE_DIR/scripts" "$HOST:/tmp/hidrowave-bridge-scripts"

ssh "${SSH_OPTS[@]}" "$HOST" "sudo mv /tmp/hidrowave-index.js $REMOTE_DIR/index.js && \
  sudo cp -r /tmp/hidrowave-bridge-scripts/* $REMOTE_DIR/scripts/ && \
  sudo chown -R \$(stat -c '%U:%G' $REMOTE_DIR) $REMOTE_DIR/index.js $REMOTE_DIR/scripts"

echo "==> Reiniciar serviço"
ssh "${SSH_OPTS[@]}" "$HOST" "sudo systemctl restart hidrowave-bridge && sleep 2 && sudo systemctl is-active hidrowave-bridge"

echo "==> Últimas linhas do journal (procure ec= sem tds=)"
ssh "${SSH_OPTS[@]}" "$HOST" "sudo journalctl -u hidrowave-bridge -n 20 --no-pager"

echo "Deploy concluído. Verifique Supabase: hydro_measurements.ec preenchido, tds NULL em inserts novos."
