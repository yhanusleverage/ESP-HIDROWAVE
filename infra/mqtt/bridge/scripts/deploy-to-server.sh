#!/usr/bin/env bash
# Deploy bridge EC único para /opt/hidrowave-bridge (Lightsail/Ubuntu)
#
# Uso (na máquina local, com chave SSH):
#   BRIDGE_HOST=ubuntu@TU_IP BRIDGE_KEY=~/sua-chave.pem ./scripts/deploy-to-server.sh
#
# Pré-requisitos Supabase (SQL Editor, nesta ordem):
#   1. HIDROWAVE-main/scripts/ADD_HYDRO_EC_COLUMN.sql
#   2. HIDROWAVE-main/scripts/HYDRO_EC_SINGLE_WRITE.sql  (trigger tds/ec_raw → null)
#
# Pré-requisitos no servidor:
#   - /opt/hidrowave-bridge com node_modules instalado
#   - .env com SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, MQTT_HOST (editar com nano, não colar no shell)

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

echo "==> Últimas linhas do journal (procure: INSERT ... ec=456 sin ec_raw= ni tds=)"
ssh "${SSH_OPTS[@]}" "$HOST" "sudo journalctl -u hidrowave-bridge -n 20 --no-pager"

cat <<'EOF'

Deploy bridge concluído.

Verificación Supabase (SQL Editor):
  SELECT created_at, ec, ec_raw, tds
  FROM hydro_measurements
  WHERE device_id = 'ESP32_HIDRO_269844'
  ORDER BY created_at DESC LIMIT 3;
  -- Esperado filas NUEVAS: ec poblado, ec_raw NULL, tds NULL

Verificación local (HIDROWAVE-main):
  npm run verify:hydro-raw

Rollback bridge:
  sudo cp /opt/hidrowave-bridge/index.js.bak.* /opt/hidrowave-bridge/index.js
  sudo systemctl restart hidrowave-bridge

Rollback trigger SQL:
  DROP TRIGGER IF EXISTS trg_hydro_ec_single_write ON public.hydro_measurements;
EOF
