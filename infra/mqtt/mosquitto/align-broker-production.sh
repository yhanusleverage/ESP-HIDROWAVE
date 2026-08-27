#!/usr/bin/env bash
# Alinha Mosquitto ACL + passwd ao master de produção atual.
#
# Uso na VM Lightsail (sudo):
#   sudo bash align-broker-production.sh ESP32_HIDRO_1A575C 'SENHA_MQTT_MASTER'
#
# Variáveis opcionais:
#   RETIRE_DEVICE=ESP32_HIDRO_269844   — remove bloco ACL + user passwd antigo
#   SKIP_BRIDGE_DEPLOY=1               — não reinicia hidrowave-bridge
#
# Depois no PC: secrets.ini → mqtt_user = mqtt_ESP32_HIDRO_1A575C + mqtt_pass → reflash ESP

set -euo pipefail

DEVICE_ID="${1:?Usage: $0 ESP32_HIDRO_XXXXXX 'mqtt_device_password'}"
DEVICE_PASS="${2:?Informe senha MQTT do device (mesma que secrets.ini mqtt_pass)}"
RETIRE_DEVICE="${RETIRE_DEVICE:-ESP32_HIDRO_269844}"
SKIP_BRIDGE_DEPLOY="${SKIP_BRIDGE_DEPLOY:-0}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "[align] Execute com sudo" >&2
  exit 1
fi

if [[ ! "$DEVICE_ID" =~ ^ESP32_HIDRO_[0-9A-F]{6}$ ]]; then
  echo "[align] device_id inválido: $DEVICE_ID" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ACL_FILE="/var/lib/mosquitto/acl"
PASSWD_FILE="/var/lib/mosquitto/passwd"
MQTT_USER="mqtt_${DEVICE_ID}"
TS="$(date +%Y%m%d%H%M%S)"

echo "[align] Backup ACL + passwd..."
cp -a "$ACL_FILE" "${ACL_FILE}.bak.${TS}"
cp -a "$PASSWD_FILE" "${PASSWD_FILE}.bak.${TS}" 2>/dev/null || true

echo "[align] Reescrevendo ACL base (bridge_internal + hidrowave Railway)..."
if [[ -f "${SCRIPT_DIR}/acl.production" ]]; then
  cp "${SCRIPT_DIR}/acl.production" "$ACL_FILE"
else
  cat > "$ACL_FILE" <<'EOF'
user bridge_internal
topic read hidrowave/+/telemetry
topic read hidrowave/+/levels
topic read hidrowave/+/heartbeat
topic read hidrowave/+/status
topic read hidrowave/+/ec_operation
topic read hidrowave/+/dose
topic read hidrowave/+/ph_operation
topic read hidrowave/+/ph_dose
topic read hidrowave/+/ec_metric
topic read hidrowave/+/ph_metric
topic read hidrowave/+/ec_dilution
topic read hidrowave/+/command_ack
topic read hidrowave/+/relay/state

user hidrowave
topic write hidrowave/+/command
topic write hidrowave/+/ec/config
topic write hidrowave/+/ph/config
topic read hidrowave/+/#

EOF
fi

echo "[align] Provisionando user device ${MQTT_USER}..."
mosquitto_passwd -b "$PASSWD_FILE" "$MQTT_USER" "$DEVICE_PASS"

MARKER="# --- device ${DEVICE_ID} (align-broker-production.sh) ---"
cat >> "$ACL_FILE" <<EOF

${MARKER}
user ${MQTT_USER}
topic read hidrowave/${DEVICE_ID}/command
topic read hidrowave/${DEVICE_ID}/ec/config
topic read hidrowave/${DEVICE_ID}/ph/config
topic write hidrowave/${DEVICE_ID}/#
EOF

if [[ -n "$RETIRE_DEVICE" && "$RETIRE_DEVICE" != "$DEVICE_ID" ]]; then
  echo "[align] Retirando device antigo: ${RETIRE_DEVICE}"
  OLD_USER="mqtt_${RETIRE_DEVICE}"
  sed -i "/# --- device ${RETIRE_DEVICE}/,/^$/d" "$ACL_FILE" 2>/dev/null || true
  sed -i "/^user ${OLD_USER}$/,/^$/d" "$ACL_FILE" 2>/dev/null || true
  mosquitto_passwd -D "$PASSWD_FILE" "$OLD_USER" 2>/dev/null || true
fi

chown mosquitto:mosquitto "$ACL_FILE" "$PASSWD_FILE"
chmod 640 "$ACL_FILE" "$PASSWD_FILE"

echo "[align] Recarregando Mosquitto..."
systemctl reload mosquitto 2>/dev/null || systemctl restart mosquitto

if [[ "$SKIP_BRIDGE_DEPLOY" != "1" ]]; then
  echo "[align] Reiniciando hidrowave-bridge..."
  systemctl restart hidrowave-bridge
fi

echo ""
echo "=== ACL alinhado ==="
grep -E '^user |topic ' "$ACL_FILE"
echo ""
echo "=== Próximo passo (PC local secrets.ini) ==="
echo "  mqtt_user = ${MQTT_USER}"
echo "  mqtt_pass = (a senha passada neste script)"
echo ""
echo "=== Verificar ==="
echo "  journalctl -u hidrowave-bridge -f"
