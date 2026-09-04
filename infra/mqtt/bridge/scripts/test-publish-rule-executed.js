/**
 * Simula rule_executed do ESP (DE local) → bridge → INSERT relay_commands
 * Uso:
 *   MQTT_USER=hidrowave MQTT_PASS=*** TEST_DEVICE_ID=ESP32_HIDRO_1A575C \
 *   TEST_RULE_ID=level_pump_on TEST_RELAY_INDEX=2 node scripts/test-publish-rule-executed.js
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const ruleId = process.env.TEST_RULE_ID || 'bench_rule';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const relayIndex = Number(process.env.TEST_RELAY_INDEX || '0');
const slaveMac = process.env.TEST_SLAVE_MAC || '';
const currentState = process.env.TEST_CURRENT_STATE !== 'false';
const success = process.env.TEST_SUCCESS !== 'false';

const eventId =
  process.env.TEST_EVENT_ID ||
  `${ruleId}-${Date.now()}-${relayIndex}`;

const topic = `hidrowave/${deviceId}/rule_executed`;

const payloadObj = {
  v: 1,
  device_id: deviceId,
  ts: Math.floor(Date.now() / 1000),
  event_id: eventId,
  rule_id: ruleId,
  relay_index: relayIndex,
  action: currentState ? 'on' : 'off',
  current_state: currentState,
  success,
  duration_s: 0,
};

if (slaveMac) {
  payloadObj.slave_mac_address = slaveMac;
}

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
    console.log(`Published rule_executed → ${topic}`);
    console.log(payload);
    console.log('\nBridge esperado:');
    console.log(`  [bridge] rule_executed INSERT relay=${relayIndex} rule=${ruleId} status=completed`);
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
