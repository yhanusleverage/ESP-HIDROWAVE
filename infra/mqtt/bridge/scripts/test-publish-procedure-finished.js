/**
 * Simula procedure_finished do ESP → bridge → INSERT procedure_events
 * Uso:
 *   MQTT_USER=hidrowave MQTT_PASS=*** TEST_DEVICE_ID=ESP32_HIDRO_1A575C \
 *   TEST_RULE_ID=fn_recarga_ate_alto TEST_STATUS=completed \
 *   node scripts/test-publish-procedure-finished.js
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const ruleId = process.env.TEST_RULE_ID || 'bench_full_recharge';
const status = process.env.TEST_STATUS === 'aborted' ? 'aborted' : 'completed';
const reason = process.env.TEST_REASON || (status === 'completed' ? 'end' : 'while_timeout');
const kind = process.env.TEST_KIND || 'full_recharge';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';

const eventId =
  process.env.TEST_EVENT_ID || `pf-${ruleId}-${Date.now()}`;

const topic = `hidrowave/${deviceId}/procedure_finished`;

const payloadObj = {
  v: 1,
  device_id: deviceId,
  ts: Math.floor(Date.now() / 1000),
  event_id: eventId,
  rule_id: ruleId,
  status,
  reason,
  kind,
};

const payload = JSON.stringify(payloadObj);

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
    console.log(`Published procedure_finished → ${topic}`);
    console.log(payload);
    console.log('\nBridge esperado:');
    console.log(
      `  [bridge] procedure_finished INSERT rule=${ruleId} status=${status}`
    );
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
