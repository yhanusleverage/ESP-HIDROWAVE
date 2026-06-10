#!/bin/bash
# Provisiona user MQTT dedicado por device no Mosquitto (Lightsail).
# Uso na VM:
#   sudo bash provision-device.sh ESP32_HIDRO_269844 'SUA_SENHA_FORTE'
#   sudo bash provision-device.sh ESP32_HIDRO_269844          # gera senha aleatória
#   sudo bash provision-device.sh ESP32_HIDRO_269844 'SENHA' --retire-hidrowave
#
# Depois: copiar mqtt_user e mqtt_pass para secrets.ini do ESP e reflash.

set -euo pipefail

DEVICE_ID="${1:?Usage: $0 ESP32_HIDRO_XXXXXX [password] [--retire-hidrowave]}"
shift || true

PASS=""
RETIRE_HIDRO=0
for arg in "$@"; do
  case "$arg" in
    --retire-hidrowave) RETIRE_HIDRO=1 ;;
    *) PASS="$arg" ;;
  esac
done

if [[ -z "$PASS" ]]; then
  PASS="$(openssl rand -base64 24 | tr -d '/+=' | head -c 20)"
  echo "[provision] Senha gerada (guarde em secrets.ini): $PASS"
fi

if [[ ! "$DEVICE_ID" =~ ^ESP32_HIDRO_[0-9A-F]{6}$ ]]; then
  echo "[provision] device_id inválido: $DEVICE_ID" >&2
  exit 1
fi

MQTT_USER="mqtt_${DEVICE_ID}"
PASSWD_FILE="/var/lib/mosquitto/passwd"
ACL_FILE="/var/lib/mosquitto/acl"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "[provision] Execute com sudo" >&2
  exit 1
fi

echo "[provision] User: $MQTT_USER"
mosquitto_passwd -b "$PASSWD_FILE" "$MQTT_USER" "$PASS"

MARKER="# --- device ${DEVICE_ID} (provision-device.sh) ---"
if grep -qF "$MARKER" "$ACL_FILE" 2>/dev/null; then
  echo "[provision] ACL já existe para $DEVICE_ID — atualizando bloco"
  sed -i "/${MARKER}/,/^$/d" "$ACL_FILE"
fi

cat >> "$ACL_FILE" <<EOF

${MARKER}
user ${MQTT_USER}
topic read hidrowave/${DEVICE_ID}/command
topic write hidrowave/${DEVICE_ID}/#
EOF

if [[ "$RETIRE_HIDRO" -eq 1 ]]; then
  echo "[provision] Removendo user lab hidrowave..."
  sed -i '/^user hidrowave$/,/^$/d' "$ACL_FILE"
  mosquitto_passwd -D "$PASSWD_FILE" hidrowave 2>/dev/null || true
fi

chown mosquitto:mosquitto "$PASSWD_FILE" "$ACL_FILE"
chmod 640 "$PASSWD_FILE" "$ACL_FILE"

systemctl reload mosquitto 2>/dev/null || systemctl restart mosquitto

echo ""
echo "=== Próximo passo (PC local) ==="
echo "secrets.ini:"
echo "  mqtt_user = ${MQTT_USER}"
echo "  mqtt_pass = ${PASS}"
echo ""
echo "Depois: platformio run -t upload && monitor"
echo ""
echo "Teste na VM:"
echo "  mosquitto_pub -h 127.0.0.1 -u ${MQTT_USER} -P '***' \\"
echo "    -t 'hidrowave/${DEVICE_ID}/telemetry' \\"
echo "    -m '{\"device_id\":\"${DEVICE_ID}\",\"ph\":6.5,\"temperature\":24.0,\"tds\":800,\"water_level_ok\":true}'"
