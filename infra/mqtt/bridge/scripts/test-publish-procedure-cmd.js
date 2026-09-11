/**
 * Envía procedure/cmd al Master (Start/Abort/Rearm) — Fase 1 FSM tanque.
 * Uso:
 *   MQTT_HOST=... MQTT_USER=... MQTT_PASS=... \
 *   TEST_DEVICE_ID=ESP32_HIDRO_1A575C TEST_RULE_ID=RULE_1788575838760 \
 *   TEST_OP=start node scripts/test-publish-procedure-cmd.js
 *
 * Serial esperado:
 *   [MQTT] procedure/cmd rule=… op=start → ok
 *   [PROC] cmd start queued …
 *   [PROC] … → Drain (start)
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_1A575C';
const ruleId = process.env.TEST_RULE_ID || 'RULE_1788575838760';
const op = (process.env.TEST_OP || 'start').toLowerCase();
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';

if (!['start', 'abort', 'rearm'].includes(op)) {
  console.error('TEST_OP must be start|abort|rearm');
  process.exit(1);
}

const topic = `hidrowave/${deviceId}/procedure/cmd`;
const payload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  rule_id: ruleId,
  op,
});

const client = mqtt.connect(`mqtt://${host}:${port}`, {
  username: process.env.MQTT_USER || process.env.MQTT_PUBLISH_USER,
  password: process.env.MQTT_PASS || process.env.MQTT_PUBLISH_PASS,
});

client.on('connect', () => {
  client.publish(topic, payload, { qos: 1 }, (err) => {
    if (err) {
      console.error('Publish failed:', err.message);
      process.exit(1);
    }
    console.log(`Published procedure/cmd → ${topic}`);
    console.log(payload);
    console.log('\nMaster esperado:');
    console.log(`  [MQTT] procedure/cmd rule=${ruleId} op=${op} → ok`);
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
