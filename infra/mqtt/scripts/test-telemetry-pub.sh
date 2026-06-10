#!/bin/bash
# Teste manual Passo 1 — publicar telemetria no Mosquitto (rodar na Lightsail ou com IP público)
# Uso: MQTT_HOST=99.x.x.x MQTT_USER=hidrowave MQTT_PASS=xxx ./test-telemetry-pub.sh [DEVICE_ID]

set -e
DEVICE_ID="${1:-ESP32_HIDRO_269844}"
HOST="${MQTT_HOST:-127.0.0.1}"
PORT="${MQTT_PORT:-1883}"
USER="${MQTT_USER:?Set MQTT_USER}"
PASS="${MQTT_PASS:?Set MQTT_PASS}"
TOPIC="hidrowave/${DEVICE_ID}/telemetry"
PAYLOAD='{"v":1,"device_id":"'"${DEVICE_ID}"'","ph":6.2,"temperature":24.5,"tds":850,"water_level_ok":true}'

mosquitto_pub -h "$HOST" -p "$PORT" -u "$USER" -P "$PASS" -t "$TOPIC" -m "$PAYLOAD"
echo "OK published to $TOPIC"
