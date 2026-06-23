/**
 * Simula command_ack do ESP → bridge → complete_relay_command
 * Uso (na VM ou PC com broker):
 *   MQTT_USER=hidrowave MQTT_PASS=*** TEST_DEVICE_ID=ESP32_HIDRO_1A575C \
 *   TEST_COMMAND_ID=200 TEST_SLAVE_MAC=14:33:5C:38:BF:60 node scripts/test-publish-command-ack.js
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const slaveMac = process.env.TEST_SLAVE_MAC || '14:33:5C:38:BF:60';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const relayIndex = Number(process.env.TEST_RELAY_INDEX || '0');
const commandId = Number(process.env.TEST_COMMAND_ID || '0');
const currentState = process.env.TEST_CURRENT_STATE !== 'false';

if (!commandId || commandId <= 0) {
  console.error('Defina TEST_COMMAND_ID=relay_commands.id (>0)');
  process.exit(1);
}

const topic = `hidrowave/${deviceId}/command_ack`;

const payload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  ts: Math.floor(Date.now() / 1000),
  id: commandId,
  status: 'completed',
  relay_index: relayIndex,
  action: currentState ? 'on' : 'off',
  current_state: currentState,
  slave_mac_address: slaveMac,
  relay_states: [true, true, false, false, true, false, false, true],
  espnow_id: 1,
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
    console.log(`Published command_ack → ${topic}`);
    console.log(payload);
    console.log('\nBridge esperado:');
    console.log(`  [bridge] RPC complete_relay_command id=${commandId}`);
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
