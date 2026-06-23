/**
 * Publica comando MQTT v1 para relé SLAVE (ESP-NOW via master).
 * Uso na VM ou PC com acesso ao broker:
 *   MQTT_USER=hidrowave MQTT_PASS=*** TEST_SLAVE_MAC=AA:BB:CC:DD:EE:FF node scripts/test-publish-slave-command.js
 *
 * Modos (TEST_MODE):
 *   instant | timed_on | timed_off | cycle | cycle_stop
 *   Ex.: TEST_MODE=cycle TEST_ON_S=10 TEST_OFF_S=5 node scripts/test-publish-slave-command.js
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_1A575C';
const slaveMac = process.env.TEST_SLAVE_MAC || 'AA:BB:CC:DD:EE:FF';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const relayIndex = Number(process.env.TEST_RELAY_INDEX || '0');
const testMode = (process.env.TEST_MODE || 'instant').toLowerCase();
const onSeconds = Number(process.env.TEST_ON_S || process.env.TEST_DURATION_S || '10');
const offSeconds = Number(process.env.TEST_OFF_S || '5');
const commandId = Number(process.env.TEST_COMMAND_ID || String(Date.now()).slice(-6));

const topic = `hidrowave/${deviceId}/command`;

function buildPayload() {
  const base = {
    v: 1,
    id: commandId,
    cmd: 'relay',
    device_id: deviceId,
    relay_index: relayIndex,
    source: 'web',
    command_type: 'manual',
    priority: 10,
    triggered_by: 'test_publish_slave',
    target_device_id: slaveMac,
    slave_mac_address: slaveMac,
  };

  switch (testMode) {
    case 'timed_on':
      return {
        ...base,
        action: 'on',
        duration_s: onSeconds,
        mode: 'timed_on',
        triggered_by: 'timer_on',
      };
    case 'timed_off':
      return {
        ...base,
        action: 'on',
        duration_s: onSeconds,
        mode: 'timed_off',
        triggered_by: 'timer_off',
      };
    case 'cycle':
      return {
        ...base,
        action: 'on',
        duration_s: onSeconds,
        mode: 'cycle',
        cycle_off_s: offSeconds,
        triggered_by: 'cycle',
      };
    case 'cycle_stop':
      return {
        ...base,
        action: 'off',
        duration_s: 0,
        mode: 'cycle_stop',
        triggered_by: 'cycle_stop',
      };
    case 'instant':
    default:
      return {
        ...base,
        action: process.env.TEST_ACTION === 'off' ? 'off' : 'on',
        duration_s: 0,
        mode: 'instant',
      };
  }
}

const payloadObj = buildPayload();
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
    console.log(`Published slave command (mode=${testMode}) → ${topic}`);
    console.log(payload);
    console.log('\nSerial master esperado:');
    console.log(`  [MQTT] rx command topic=${topic}`);
    console.log(
      `  [CMD mqtt] id=${commandId} slave R${relayIndex} ${payloadObj.action} mode=${payloadObj.mode ?? 'instant'} … tgt=${slaveMac}`
    );
    if (testMode === 'cycle') {
      console.log(`  [COORD] SLAVE on relay ${relayIndex + 1} dur=${onSeconds}s (ciclo OFF ${offSeconds}s)`);
      console.log('  Slave serial: Ciclo iniciado ON Xs / OFF Ys');
    }
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
