/**
 * HIDROWAVE Bridge — telemetria + heartbeat + presença + Auto EC UX
 * Subscribe:
 *   hidrowave/+/telemetry     → INSERT hydro_measurements + environment_data
 *   hidrowave/+/heartbeat       → PATCH device_status
 *   hidrowave/+/status          → PATCH device_status.is_online (LWT)
 *   hidrowave/+/ec_operation    → PATCH relay_master.ec_operation_*
 *   hidrowave/+/dose            → INSERT nutrient_dosages
 */
import 'dotenv/config';
import mqtt from 'mqtt';
import ws from 'ws';
import { createClient } from '@supabase/supabase-js';

const DEVICE_ID_RE = /^ESP32_HIDRO_[0-9A-F]{6}$/;

const TOPICS = [
  'hidrowave/+/telemetry',
  'hidrowave/+/heartbeat',
  'hidrowave/+/status',
  'hidrowave/+/ec_operation',
  'hidrowave/+/dose',
];

const EC_OPERATION_STATES = new Set([
  'idle',
  'dosing',
  'waiting_nutrient',
  'recirculating',
  'ec_check_pending',
]);

const DOSE_SOURCES = new Set(['auto_ec', 'manual', 'web']);

const telemetryThrottleMs = parseInt(process.env.TELEMETRY_THROTTLE_MS || '30000', 10);
const heartbeatThrottleMs = parseInt(process.env.HEARTBEAT_THROTTLE_MS || '55000', 10);
const heartbeatStaleMs = parseInt(process.env.HEARTBEAT_STALE_MS || '120000', 10);
const ecOperationThrottleMs = parseInt(process.env.EC_OPERATION_THROTTLE_MS || '2000', 10);

// Alinhado com CHECK Supabase: environment_data_temperature_check / environment_data_humidity_check
const ENV_TEMP_MIN = 0;
const ENV_TEMP_MAX = 50;
const ENV_HUMIDITY_MIN = 0;
const ENV_HUMIDITY_MAX = 100;

const lastTelemetryInsertByDevice = new Map();
const lastHeartbeatUpsertByDevice = new Map();
const lastHeartbeatAtByDevice = new Map();
const lastEcOperationSnapshotByDevice = new Map();

function requireEnv(name) {
  const v = process.env[name];
  if (!v) {
    console.error(`[bridge] Missing env: ${name}`);
    process.exit(1);
  }
  return v;
}

const supabase = createClient(
  requireEnv('SUPABASE_URL'),
  requireEnv('SUPABASE_SERVICE_ROLE_KEY'),
  {
    auth: { persistSession: false, autoRefreshToken: false },
    realtime: { transport: ws },
  }
);

const mqttHost = process.env.MQTT_HOST || '127.0.0.1';
const mqttPort = process.env.MQTT_PORT || '1883';
const mqttUrl = `mqtt://${mqttHost}:${mqttPort}`;

function parseDeviceIdFromTopic(topic, expectedSuffix) {
  const parts = topic.split('/');
  if (parts.length !== 3 || parts[0] !== 'hidrowave' || parts[2] !== expectedSuffix) {
    return null;
  }
  return parts[1];
}

function topicSuffix(topic) {
  const parts = topic.split('/');
  return parts.length === 3 ? parts[2] : null;
}

function isValidDeviceId(deviceId) {
  return DEVICE_ID_RE.test(deviceId);
}

function checkDeviceIdMatch(deviceId, payload) {
  if (payload.device_id && payload.device_id !== deviceId) {
    return { ok: false, reason: 'device_id mismatch topic vs json' };
  }
  return { ok: true };
}

function validateEnvironmentFields(airTemp, humidity) {
  if (Number.isNaN(airTemp) || Number.isNaN(humidity)) {
    return {
      ok: false,
      reason: 'air_temp/humidity missing or not a number',
    };
  }
  if (airTemp < ENV_TEMP_MIN || airTemp > ENV_TEMP_MAX) {
    return {
      ok: false,
      reason: `temperature ${airTemp} outside [${ENV_TEMP_MIN}, ${ENV_TEMP_MAX}] (environment_data_temperature_check)`,
      field: 'temperature',
      value: airTemp,
      min: ENV_TEMP_MIN,
      max: ENV_TEMP_MAX,
    };
  }
  if (humidity < ENV_HUMIDITY_MIN || humidity > ENV_HUMIDITY_MAX) {
    return {
      ok: false,
      reason: `humidity ${humidity} outside [${ENV_HUMIDITY_MIN}, ${ENV_HUMIDITY_MAX}] (environment_data_humidity_check)`,
      field: 'humidity',
      value: humidity,
      min: ENV_HUMIDITY_MIN,
      max: ENV_HUMIDITY_MAX,
    };
  }
  return {
    ok: true,
    row: {
      temperature: airTemp,
      humidity,
    },
  };
}

function logEnvironmentValues(deviceId, airTemp, humidity) {
  const tempStr = Number.isNaN(airTemp) ? 'n/a' : airTemp;
  const humStr = Number.isNaN(humidity) ? 'n/a' : humidity;
  console.log(
    `[bridge] environment_data ${deviceId} temp=${tempStr} hum=${humStr} ` +
      `(restricões temp ${ENV_TEMP_MIN}-${ENV_TEMP_MAX}, hum ${ENV_HUMIDITY_MIN}-${ENV_HUMIDITY_MAX})`
  );
}

function validateTelemetry(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const temperature = Number(payload.temperature);
  const ph = Number(payload.ph);
  const tds = Number(payload.tds);
  const waterLevelOk = payload.water_level_ok;

  if (Number.isNaN(temperature) || Number.isNaN(ph) || Number.isNaN(tds)) {
    return { ok: false, reason: 'temperature/ph/tds must be numbers' };
  }
  if (typeof waterLevelOk !== 'boolean') {
    return { ok: false, reason: 'water_level_ok must be boolean' };
  }

  const airTempRaw =
    payload.air_temp != null ? Number(payload.air_temp) : temperature;
  const humidityRaw = payload.humidity != null ? Number(payload.humidity) : NaN;

  let envRow = null;
  let envSkip = null;
  const envCheck = validateEnvironmentFields(airTempRaw, humidityRaw);
  if (envCheck.ok) {
    envRow = {
      device_id: deviceId,
      temperature: envCheck.row.temperature,
      humidity: envCheck.row.humidity,
    };
  } else if (payload.air_temp != null || payload.humidity != null) {
    envSkip = envCheck;
  }

  return {
    ok: true,
    row: {
      device_id: deviceId,
      temperature,
      ph,
      tds,
      water_level_ok: waterLevelOk,
    },
    envRow,
    envSkip,
    envReceived:
      payload.air_temp != null || payload.humidity != null
        ? { airTemp: airTempRaw, humidity: humidityRaw }
        : null,
  };
}

function validateHeartbeat(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const wifiRssi = Number(payload.wifi_rssi);
  const freeHeap = Number(payload.free_heap);
  const uptimeSeconds = Number(payload.uptime_seconds);
  const rebootCount = Number(payload.reboot_count);

  if (!Number.isInteger(wifiRssi)) {
    return { ok: false, reason: 'wifi_rssi must be integer' };
  }
  if (!Number.isFinite(freeHeap) || freeHeap < 0) {
    return { ok: false, reason: 'free_heap must be non-negative number' };
  }
  if (!Number.isFinite(uptimeSeconds) || uptimeSeconds < 0) {
    return { ok: false, reason: 'uptime_seconds must be non-negative number' };
  }
  if (!Number.isInteger(rebootCount) || rebootCount < 0) {
    return { ok: false, reason: 'reboot_count must be non-negative integer' };
  }

  const firmwareVersion =
    payload.firmware_version != null ? String(payload.firmware_version) : null;
  const ipAddress =
    payload.ip_address != null && String(payload.ip_address).length > 0
      ? String(payload.ip_address)
      : null;

  return {
    ok: true,
    row: {
      device_id: deviceId,
      wifi_rssi: wifiRssi,
      free_heap: Math.floor(freeHeap),
      uptime_seconds: Math.floor(uptimeSeconds),
      reboot_count: rebootCount,
      firmware_version: firmwareVersion,
      ip_address: ipAddress,
    },
  };
}

function validateEcOperation(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const state = String(payload.ec_operation_state || '');
  if (!EC_OPERATION_STATES.has(state)) {
    return { ok: false, reason: `invalid ec_operation_state: ${state}` };
  }

  const remaining = Number(payload.ec_operation_remaining_sec);
  const nextCheck = Number(payload.ec_next_check_in_sec);
  if (!Number.isFinite(remaining) || remaining < 0) {
    return { ok: false, reason: 'ec_operation_remaining_sec must be >= 0' };
  }
  if (!Number.isFinite(nextCheck) || nextCheck < 0) {
    return { ok: false, reason: 'ec_next_check_in_sec must be >= 0' };
  }

  return {
    ok: true,
    row: {
      device_id: deviceId,
      ec_operation_state: state,
      ec_operation_remaining_sec: Math.floor(remaining),
      ec_next_check_in_sec: Math.floor(nextCheck),
    },
  };
}

function validateDose(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const sequenceId = String(payload.sequence_id || '').trim();
  const nutrientName = String(payload.nutrient_name || '');
  const relayNumber = Number(payload.relay_number);
  const dosageMl = Number(payload.dosage_ml);
  const dosageTimeSeconds = Number(payload.dosage_time_seconds);
  const source = String(payload.source || 'auto_ec');

  if (!sequenceId) {
    return { ok: false, reason: 'sequence_id required' };
  }
  if (!Number.isInteger(relayNumber) || relayNumber < 0 || relayNumber > 15) {
    return { ok: false, reason: 'relay_number must be 0-15' };
  }
  if (!Number.isFinite(dosageMl) || dosageMl < 0) {
    return { ok: false, reason: 'dosage_ml must be >= 0' };
  }
  if (!Number.isFinite(dosageTimeSeconds) || dosageTimeSeconds < 0) {
    return { ok: false, reason: 'dosage_time_seconds must be >= 0' };
  }
  if (!DOSE_SOURCES.has(source)) {
    return { ok: false, reason: `invalid source: ${source}` };
  }

  const row = {
    device_id: deviceId,
    sequence_id: sequenceId,
    nutrient_name: nutrientName,
    relay_number: relayNumber,
    dosage_ml: Math.round(dosageMl * 1000) / 1000,
    dosage_time_seconds: Math.round(dosageTimeSeconds * 100) / 100,
    source,
  };

  if (payload.ec_before != null && Number.isFinite(Number(payload.ec_before))) {
    row.ec_before = Math.round(Number(payload.ec_before) * 100) / 100;
  }
  if (payload.ec_setpoint != null && Number.isFinite(Number(payload.ec_setpoint))) {
    row.ec_setpoint = Math.round(Number(payload.ec_setpoint) * 100) / 100;
  }

  return { ok: true, row };
}

function validateStatus(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  if (typeof payload.online !== 'boolean') {
    return { ok: false, reason: 'online must be boolean' };
  }

  return { ok: true, online: payload.online };
}

function shouldThrottle(map, deviceId, intervalMs) {
  const now = Date.now();
  const last = map.get(deviceId) || 0;
  if (now - last < intervalMs) {
    return true;
  }
  map.set(deviceId, now);
  return false;
}

function shouldThrottleEcOperation(deviceId, row) {
  const prev = lastEcOperationSnapshotByDevice.get(deviceId);
  const now = Date.now();
  if (!prev) {
    return false;
  }
  if (prev.state !== row.ec_operation_state) {
    return false;
  }
  if (Math.abs(prev.remaining - row.ec_operation_remaining_sec) > 2) {
    return false;
  }
  return now - prev.at < ecOperationThrottleMs;
}

function rememberEcOperation(deviceId, row) {
  lastEcOperationSnapshotByDevice.set(deviceId, {
    state: row.ec_operation_state,
    remaining: row.ec_operation_remaining_sec,
    at: Date.now(),
  });
}

async function insertHydro(row) {
  const { error } = await supabase.from('hydro_measurements').insert(row);
  if (error) {
    console.error(`[bridge] Supabase insert failed (${row.device_id}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] INSERT hydro_measurements ${row.device_id} ph=${row.ph} temp=${row.temperature} tds=${row.tds}`
  );
  return true;
}

async function insertEnvironment(row) {
  const preCheck = validateEnvironmentFields(row.temperature, row.humidity);
  if (!preCheck.ok) {
    return false;
  }

  const { error } = await supabase.from('environment_data').insert(row);
  if (error) {
    console.error(
      `[bridge] environment_data insert failed (${row.device_id}): ${error.message} | ` +
        `temp=${row.temperature} hum=${row.humidity}`
    );
    return false;
  }
  console.log(`[bridge] INSERT environment_data ${row.device_id} OK`);
  return true;
}

async function patchDeviceHealth(row) {
  const nowIso = new Date().toISOString();
  const patch = {
    wifi_rssi: row.wifi_rssi,
    free_heap: row.free_heap,
    uptime_seconds: row.uptime_seconds,
    reboot_count: row.reboot_count,
    firmware_version: row.firmware_version,
    ip_address: row.ip_address,
    last_seen: nowIso,
    is_online: true,
    updated_at: nowIso,
  };

  const { error } = await supabase
    .from('device_status')
    .update(patch)
    .eq('device_id', row.device_id);

  if (error) {
    console.error(`[bridge] device_status health patch failed (${row.device_id}):`, error.message);
    return false;
  }

  lastHeartbeatAtByDevice.set(row.device_id, Date.now());
  console.log(
    `[bridge] PATCH device_status ${row.device_id} heap=${row.free_heap} rssi=${row.wifi_rssi} reboot=${row.reboot_count}`
  );
  return true;
}

async function patchEcOperation(row) {
  const patch = {
    ec_operation_state: row.ec_operation_state,
    ec_operation_remaining_sec: row.ec_operation_remaining_sec,
    ec_next_check_in_sec: row.ec_next_check_in_sec,
  };

  const { error } = await supabase
    .from('relay_master')
    .update(patch)
    .eq('device_id', row.device_id);

  if (error) {
    console.error(`[bridge] relay_master ec_operation patch failed (${row.device_id}):`, error.message);
    return false;
  }

  console.log(
    `[bridge] PATCH relay_master ${row.device_id} ec_operation=${row.ec_operation_state} rem=${row.ec_operation_remaining_sec}s`
  );
  return true;
}

async function insertDose(row) {
  const { error } = await supabase.from('nutrient_dosages').insert(row);
  if (error) {
    console.error(`[bridge] nutrient_dosages insert failed (${row.device_id}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] INSERT nutrient_dosages ${row.device_id} ${row.nutrient_name} ${row.dosage_ml}ml seq=${row.sequence_id}`
  );
  return true;
}

async function patchOnline(deviceId, online) {
  const patch = {
    is_online: online,
    last_seen: new Date().toISOString(),
  };

  const { error } = await supabase
    .from('device_status')
    .update(patch)
    .eq('device_id', deviceId);

  if (error) {
    console.error(`[bridge] status patch failed (${deviceId}):`, error.message);
    return false;
  }

  if (online) {
    lastHeartbeatAtByDevice.set(deviceId, Date.now());
  } else {
    lastHeartbeatAtByDevice.delete(deviceId);
  }

  console.log(`[bridge] device_status ${deviceId} is_online=${online}`);
  return true;
}

async function handleTelemetry(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'telemetry');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateTelemetry(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  if (shouldThrottle(lastTelemetryInsertByDevice, deviceId, telemetryThrottleMs)) {
    console.log(`[bridge] Throttled telemetry ${deviceId} (< ${telemetryThrottleMs}ms)`);
    return;
  }

  await insertHydro(validated.row);
  if (validated.envReceived) {
    logEnvironmentValues(
      deviceId,
      validated.envReceived.airTemp,
      validated.envReceived.humidity
    );
  }
  if (validated.envRow) {
    await insertEnvironment(validated.envRow);
  }
}

async function handleHeartbeat(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'heartbeat');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateHeartbeat(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  if (shouldThrottle(lastHeartbeatUpsertByDevice, deviceId, heartbeatThrottleMs)) {
    console.log(`[bridge] Throttled heartbeat ${deviceId} (< ${heartbeatThrottleMs}ms)`);
    return;
  }

  await patchDeviceHealth(validated.row);
}

async function handleEcOperation(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'ec_operation');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateEcOperation(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  if (shouldThrottleEcOperation(deviceId, validated.row)) {
    console.log(`[bridge] Throttled ec_operation ${deviceId} (redundant)`);
    return;
  }

  const ok = await patchEcOperation(validated.row);
  if (ok) {
    rememberEcOperation(deviceId, validated.row);
  }
}

async function handleDose(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'dose');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateDose(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  await insertDose(validated.row);
}

async function handleStatus(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'status');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateStatus(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  await patchOnline(deviceId, validated.online);
}

async function markStaleDevicesOffline() {
  const now = Date.now();
  for (const [deviceId, lastAt] of lastHeartbeatAtByDevice.entries()) {
    if (now - lastAt >= heartbeatStaleMs) {
      console.warn(`[bridge] Stale heartbeat ${deviceId} — marking offline`);
      await patchOnline(deviceId, false);
    }
  }
}

const client = mqtt.connect(mqttUrl, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASS,
  reconnectPeriod: 5000,
  connectTimeout: 30000,
});

client.on('connect', () => {
  console.log(`[bridge] Connected to ${mqttUrl}`);
  client.subscribe(TOPICS, { qos: 0 }, (err) => {
    if (err) {
      console.error('[bridge] Subscribe failed:', err.message);
      process.exit(1);
    }
    console.log(
      `[bridge] Subscribed ${TOPICS.join(', ')} | telemetry ${telemetryThrottleMs}ms | heartbeat ${heartbeatThrottleMs}ms | ec_operation ${ecOperationThrottleMs}ms | stale ${heartbeatStaleMs}ms`
    );
  });
});

client.on('message', (topic, message) => {
  const suffix = topicSuffix(topic);
  const run = async () => {
    if (suffix === 'telemetry') {
      await handleTelemetry(topic, message);
    } else if (suffix === 'heartbeat') {
      await handleHeartbeat(topic, message);
    } else if (suffix === 'status') {
      await handleStatus(topic, message);
    } else if (suffix === 'ec_operation') {
      await handleEcOperation(topic, message);
    } else if (suffix === 'dose') {
      await handleDose(topic, message);
    }
  };
  run().catch((e) => {
    console.error('[bridge] Handler error:', e.message);
  });
});

client.on('error', (err) => {
  console.error('[bridge] MQTT error:', err.message);
});

setInterval(() => {
  markStaleDevicesOffline().catch((e) => {
    console.error('[bridge] Stale check error:', e.message);
  });
}, Math.min(heartbeatStaleMs, 60000));

process.on('SIGINT', () => {
  client.end(true, () => process.exit(0));
});
