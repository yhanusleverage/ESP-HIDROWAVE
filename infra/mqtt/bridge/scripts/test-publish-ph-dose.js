/**
 * Publica ph_operation + ph_dose de teste (validar broker + bridge + Supabase).
 * Uso: npm run test:pub:ph-dose
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const sequenceId = process.env.TEST_SEQUENCE_ID || `test-ph-mqtt-${Date.now()}`;

const phOpTopic = `hidrowave/${deviceId}/ph_operation`;
const phDoseTopic = `hidrowave/${deviceId}/ph_dose`;

const phOpPayload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  ph_operation_state: 'recirculating',
  ph_operation_remaining_sec: 45,
  ph_next_check_in_sec: 0,
});

const phDosePayload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  sequence_id: sequenceId,
  direction: 'up',
  relay_number: 1,
  dosage_ml: 2.5,
  dosage_time_seconds: 3,
  ph_before: 5.8,
  ph_setpoint: 6.0,
  source: 'auto_ph',
});

const client = mqtt.connect(`mqtt://${host}:${port}`, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASS,
});

client.on('connect', () => {
  client.publish(phOpTopic, phOpPayload, { qos: 0 }, (errOp) => {
    if (errOp) {
      console.error('ph_operation publish failed:', errOp.message);
      process.exit(1);
    }
    console.log(`Published ${phOpTopic}:`, phOpPayload);

    client.publish(phDoseTopic, phDosePayload, { qos: 1 }, (errDose) => {
      if (errDose) {
        console.error('ph_dose publish failed:', errDose.message);
        process.exit(1);
      }
      console.log(`Published ${phDoseTopic}:`, phDosePayload);
      console.log('Verificar: relay_master.ph_operation_* y ph_dosages en Supabase');
      client.end();
    });
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
