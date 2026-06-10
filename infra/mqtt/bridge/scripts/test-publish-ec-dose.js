/**
 * Publica ec_operation + dose de teste (validar broker + bridge + Supabase).
 * Uso: npm run test:pub:ec-dose
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const sequenceId = process.env.TEST_SEQUENCE_ID || `test-mqtt-${Date.now()}`;

const ecTopic = `hidrowave/${deviceId}/ec_operation`;
const doseTopic = `hidrowave/${deviceId}/dose`;

const ecPayload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  ec_operation_state: 'recirculating',
  ec_operation_remaining_sec: 60,
  ec_next_check_in_sec: 0,
});

const dosePayload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  sequence_id: sequenceId,
  nutrient_name: '22CCC',
  relay_number: 3,
  dosage_ml: 10.5,
  dosage_time_seconds: 5,
  ec_before: 850,
  ec_setpoint: 1200,
  source: 'auto_ec',
});

const client = mqtt.connect(`mqtt://${host}:${port}`, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASS,
});

client.on('connect', () => {
  client.publish(ecTopic, ecPayload, { qos: 0 }, (errEc) => {
    if (errEc) {
      console.error('ec_operation publish failed:', errEc.message);
      process.exit(1);
    }
    console.log(`Published ${ecTopic}:`, ecPayload);

    client.publish(doseTopic, dosePayload, { qos: 1 }, (errDose) => {
      if (errDose) {
        console.error('dose publish failed:', errDose.message);
        process.exit(1);
      }
      console.log(`Published ${doseTopic}:`, dosePayload);
      console.log('Verificar: relay_master.ec_operation_* y nutrient_dosages en Supabase');
      client.end();
    });
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
