/**
 * Publica comando MQTT v1 para relé SLAVE (ESP-NOW via master).
 * Uso na VM ou PC com acesso ao broker:
 *   MQTT_USER=hidrowave MQTT_PASS=*** TEST_SLAVE_MAC=AA:BB:CC:DD:EE:FF node scripts/test-publish-slave-command.js
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const slaveMac = process.env.TEST_SLAVE_MAC || 'AA:BB:CC:DD:EE:FF';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const relayIndex = Number(process.env.TEST_RELAY_INDEX || '0');
const action = process.env.TEST_ACTION === 'off' ? 'off' : 'on';
const commandId = Number(process.env.TEST_COMMAND_ID || String(Date.now()).slice(-6));

const topic = `hidrowave/${deviceId}/command`;

const payload = JSON.stringify({
  v: 1,
  id: commandId,
  cmd: 'relay',
  device_id: deviceId,
  relay_index: relayIndex,
  action,
  duration_s: 0,
  target_device_id: slaveMac,
  slave_mac_address: slaveMac,
  source: 'web',
  command_type: 'manual',
  priority: 10,
  triggered_by: 'test_publish_slave',
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
    console.log(`Published slave command → ${topic}`);
    console.log(payload);
    console.log('\nSerial master esperado:');
    console.log(`  [MQTT] rx command topic=${topic}`);
    console.log(`  [CMD mqtt] id=${commandId} slave R${relayIndex} ${action} … tgt=${slaveMac}`);
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
