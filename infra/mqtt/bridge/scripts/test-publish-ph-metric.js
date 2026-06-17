/**
 * Publica ph_metric de teste (validar broker + bridge + Supabase ph_controller_metrics).
 * Uso: npm run test:pub:ph-metric
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_269844';
const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const sequenceId = process.env.TEST_SEQUENCE_ID || `test-ph-metric-${Date.now()}`;

const metricTopic = `hidrowave/${deviceId}/ph_metric`;

const metricPayload = JSON.stringify({
  v: 1,
  device_id: deviceId,
  ph_setpoint: 6.2,
  ph_before: 5.85,
  error_h: 1.234e-5,
  direction: 'up',
  k_acid: 0.02,
  k_base: 0.025,
  k_used: 0.025,
  dose_ideal_ml: 2.5,
  dose_real_ml: 2.5,
  dosage_time_seconds: 5,
  aggressiveness: 1.0,
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
      console.error('ph_metric publish failed:', err.message);
      process.exit(1);
    }
    console.log(`Published ${metricTopic}:`, metricPayload);
    console.log('Verificar: journalctl INSERT ph_controller_metrics + npm run verify:controller-metrics');
    client.end();
  });
});

client.on('error', (e) => {
  console.error(e.message);
  process.exit(1);
});
