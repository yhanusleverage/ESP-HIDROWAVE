/**
 * Publica ec_metric de teste (validar broker + bridge + Supabase ec_controller_metrics).
 * Uso: npm run test:pub:ec-metric
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const sequenceId = process.env.TEST_SEQUENCE_ID || `test-metric-${Date.now()}`;

const metricTopic = `hidrowave/${deviceId}/ec_metric`;

const metricPayload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  ec_setpoint: 1200,
  ec_actual: 850,
  ec_error: 350,
  k_value: 0.5,
  dosage_ml: 4.28,
  dosage_time_seconds: 8.5,
  base_dose: 800,
  flow_rate: 0.5,
  volume: 100,
  total_ml: 50,
  kp: 1.0,
  auto_enabled: true,
  adjustment_needed: true,
  adjustment_applied: false,
  sequence_id: sequenceId,
});

const client = mqtt.connect(`mqtt://${host}:${port}`, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASS,
});

client.on('connect', () => {
  client.publish(metricTopic, metricPayload, { qos: 0 }, (err) => {
    if (err) {
      console.error('ec_metric publish failed:', err.message);
      process.exit(1);
    }
    console.log(`Published ${metricTopic}:`, metricPayload);
    console.log('Verificar: journalctl INSERT ec_controller_metrics + npm run verify:controller-metrics');
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
