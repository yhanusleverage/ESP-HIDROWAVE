/**
 * Simula ec_gain do ESP → bridge → PATCH ec_config_view.k_value
 *
 *   TEST_DEVICE_ID=ESP32_HIDRO_1A575C TEST_K_VALUE=0.3721 node scripts/test-publish-ec-gain.js
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || process.env.DEVICE_ID;
const kValue = Number(process.env.TEST_K_VALUE || '0.3721');

if (!deviceId) {
  console.error('Defina TEST_DEVICE_ID=ESP32_HIDRO_XXXXXX');
  process.exit(1);
}

const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const user = process.env.MQTT_USER || 'hidrowave';
const pass = process.env.MQTT_PASS || '';

const topic = `hidrowave/${deviceId}/ec_gain`;
const payload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  ts: Math.floor(Date.now() / 1000),
  k_value: kValue,
});

const client = mqtt.connect(`mqtt://${host}:${port}`, {
  username: user,
  password: pass,
});

client.on('connect', () => {
  client.publish(topic, payload, { qos: 0 }, (err) => {
    if (err) {
      console.error('Publish failed:', err.message);
      process.exit(1);
    }
    console.log(`Published ec_gain → ${topic}`);
    console.log(payload);
    client.end();
  });
});
