/**
 * Publica uma mensagem de telemetria de teste (validar broker + ACL).
 * Uso: node scripts/test-publish.js
 * Requer .env com MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const topic = `hidrowave/${deviceId}/telemetry`;

const payload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  ph: 6.2,
  temperature: 24.5,
  tds: 850,
  water_level_ok: true,
});

const client = mqtt.connect(`mqtt://${host}:${port}`, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASS,
});

client.on('connect', () => {
  client.publish(topic, payload, { qos: 0 }, (err) => {
    if (err) {
      console.error('Publish failed:', err.message);
      process.exit(1);
    }
    console.log(`Published to ${topic}:`, payload);
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
