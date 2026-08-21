#!/usr/bin/env bash
# Borra LWT/dosis MQTT retenidas (payload vacío + retain).
# El user MQTT debe tener write en esos tópicos (mqtt_ESP32_HIDRO_XXXXXX para su device).
#
# Uso en la VM:
#   MQTT_USER=mqtt_ESP32_HIDRO_1A575C MQTT_PASS='...' \
#     sudo -E bash clear-retained-presence.sh ESP32_HIDRO_1A575C
#   MQTT_USER=mqtt_ESP32_HIDRO_269844 MQTT_PASS='...' \
#     sudo -E bash clear-retained-presence.sh ESP32_HIDRO_269844
#
# 269844 solo si el user sigue en passwd/ACL (align lo retira por defecto).

set -euo pipefail

DEVICE_ID="${1:?Usage: $0 ESP32_HIDRO_XXXXXX}"
MQTT_HOST="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT="${MQTT_PORT:-1883}"
MQTT_USER="${MQTT_USER:?Set MQTT_USER (device user with write on hidrowave/${DEVICE_ID}/#)}"
MQTT_PASS="${MQTT_PASS:?Set MQTT_PASS}"

if [[ ! "$DEVICE_ID" =~ ^ESP32_HIDRO_[0-9A-F]{6}$ ]]; then
  echo "[clear-retain] device_id inválido: $DEVICE_ID" >&2
  exit 1
fi

topics=(
  "hidrowave/${DEVICE_ID}/status"
  "hidrowave/${DEVICE_ID}/dose"
  "hidrowave/${DEVICE_ID}/ph_dose"
)

for t in "${topics[@]}"; do
  echo "[clear-retain] $t"
  mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -u "$MQTT_USER" -P "$MQTT_PASS" -r -n -t "$t"
done

echo "[clear-retain] OK ${DEVICE_ID}"
