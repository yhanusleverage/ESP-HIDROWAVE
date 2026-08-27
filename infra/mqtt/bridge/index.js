/**
 * HIDROWAVE Bridge â€” telemetria + heartbeat + presenÃ§a + Auto EC UX
 * Subscribe:
 *   hidrowave/+/telemetry     → INSERT hydro_measurements + environment_data
 *   hidrowave/+/levels          → PATCH device_status level_* (on-change, sin throttle 30s)
 *   hidrowave/+/heartbeat       → PATCH device_status
 *   hidrowave/+/status          → PATCH device_status.is_online (LWT)
 *   hidrowave/+/ec_operation    → PATCH relay_master.ec_operation_*
 *   hidrowave/+/dose            → INSERT nutrient_dosages
 *   hidrowave/+/ph_operation    → PATCH relay_master.ph_operation_*
 *   hidrowave/+/ph_dose         → INSERT ph_dosages
 *   hidrowave/+/ec_metric       → INSERT ec_controller_metrics
 *   hidrowave/+/ph_metric       → INSERT ph_controller_metrics
 */
import 'dotenv/config';
import mqtt from 'mqtt';
import ws from 'ws';
import { createClient } from '@supabase/supabase-js';

const DEVICE_ID_RE = /^ESP32_HIDRO_[0-9A-F]{6}$/;
const SLAVE_MAC_RE = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

const TOPICS = [
  'hidrowave/+/telemetry',
  'hidrowave/+/levels',
  'hidrowave/+/heartbeat',
  'hidrowave/+/status',
  'hidrowave/+/ec_operation',
  'hidrowave/+/dose',
  'hidrowave/+/ph_operation',
  'hidrowave/+/ph_dose',
  'hidrowave/+/ec_metric',
  'hidrowave/+/ph_metric',
  'hidrowave/+/ec_dilution',
  'hidrowave/+/command_ack',
  'hidrowave/+/relay/state',
];

const EC_OPERATION_STATES = new Set([
  'idle',
  'dosing',
  'waiting_nutrient',
  'recirculating',
  'ec_check_pending',
  'diluting_draining',
  'diluting_filling',
]);

const EC_DILUTION_SOURCES = new Set(['auto', 'manual', 'web']);

const DOSE_SOURCES = new Set(['auto_ec', 'manual', 'web']);
const WATER_LEVEL_VALUES = new Set([
  'vazio',
  'baixo',
  'medio',
  'medio_alto',
  'alto',
  'medio_baixo', // aceptado → normaliza a medio
]);
const INTERLOCK_MODE_VALUES = new Set(['normal', 'carrera']);

/** Canonical: 2/4=medio, 3/4=medio_alto. medio_baixo → medio. */
function normalizeWaterLevel(raw) {
  if (raw == null) return null;
  const s = String(raw);
  if (s === 'medio_baixo') return 'medio';
  if (WATER_LEVEL_VALUES.has(s) && s !== 'medio_baixo') return s;
  return null;
}

function normalizeInterlockMode(raw) {
  if (raw == null) return null;
  const s = String(raw).toLowerCase();
  return INTERLOCK_MODE_VALUES.has(s) ? s : null;
}

const PH_OPERATION_STATES = new Set([
  'idle',
  'dosing',
  'recirculating',
  'ph_check_pending',
]);

const PH_DOSE_SOURCES = new Set(['auto_ph', 'manual', 'web']);
const PH_DOSE_DIRECTIONS = new Set(['up', 'down']);

const telemetryThrottleMs = parseInt(process.env.TELEMETRY_THROTTLE_MS || '30000', 10);
const levelsEventThrottleMs = parseInt(process.env.LEVELS_EVENT_THROTTLE_MS || '300', 10);
const heartbeatThrottleMs = parseInt(process.env.HEARTBEAT_THROTTLE_MS || '55000', 10);
const heartbeatStaleMs = parseInt(process.env.HEARTBEAT_STALE_MS || '120000', 10);
const ecOperationThrottleMs = parseInt(process.env.EC_OPERATION_THROTTLE_MS || '2000', 10);
const phOperationThrottleMs = parseInt(process.env.PH_OPERATION_THROTTLE_MS || '2000', 10);
const relayStateThrottleMs = parseInt(process.env.RELAY_STATE_THROTTLE_MS || '1000', 10);
const relayStateCoalesceMs = parseInt(process.env.RELAY_STATE_COALESCE_MS || '300', 10);
const relayHeartbeatThrottleMs = parseInt(process.env.RELAY_HEARTBEAT_THROTTLE_MS || '45000', 10);

const COMMAND_ACK_STATUSES = new Set(['completed', 'failed']);

// Alinhado com CHECK Supabase: environment_data_temperature_check / environment_data_humidity_check
const ENV_TEMP_MIN = 0;
const ENV_TEMP_MAX = 50;
const ENV_HUMIDITY_MIN = 0;
const ENV_HUMIDITY_MAX = 100;

const RETAIN_SUBSCRIBE_GRACE_MS = parseInt(process.env.RETAIN_SUBSCRIBE_GRACE_MS || '3000', 10);
let subscribedAt = 0;

const lastTelemetryInsertByDevice = new Map();
const lastLevelsEventByDevice = new Map();
const lastHeartbeatUpsertByDevice = new Map();
const lastHeartbeatAtByDevice = new Map();
const lastEcOperationSnapshotByDevice = new Map();
const lastPhOperationSnapshotByDevice = new Map();
const lastRelayStatePatchByDevice = new Map();
const lastRelayHeartbeatPatchByDevice = new Map();
const pendingRelayStatePatches = new Map();
const completedCommandAckIds = new Map();

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
      `(restricÃµes temp ${ENV_TEMP_MIN}-${ENV_TEMP_MAX}, hum ${ENV_HUMIDITY_MIN}-${ENV_HUMIDITY_MAX})`
  );
}

/** ph_controller_metrics.ph_before es numeric(8,3) â€” clamp PV RS485 basura en banco */
function clampPhMetricColumn(ph) {
  if (!Number.isFinite(ph)) return 0;
  const MAG = 99.999;
  if (ph > MAG) return MAG;
  if (ph < -MAG) return -MAG;
  return Math.round(ph * 1000) / 1000;
}

function sanitizeErrorH(phBefore, phSetpoint, errorH) {
  let h = Number(errorH ?? 0);
  if (!Number.isFinite(h)) {
    h = (clampPhMetricColumn(phBefore) - clampPhMetricColumn(phSetpoint)) * 1e-6;
  }
  if (!Number.isFinite(h)) return 0;
  const MAX = 1e6;
  if (h > MAX) return MAX;
  if (h < -MAX) return -MAX;
  return h;
}

/** pH clamp 0â€“14 para grÃ¡ficos (ph_display_clamped); ph_raw guarda PV MQTT crudo. */
function clampPhDisplay(ph) {
  if (!Number.isFinite(ph)) return null;
  if (ph < 0) return 0;
  if (ph > 14) return 14;
  return Math.round(ph * 1000) / 1000;
}

function resolveEcUsCmFromPayload({ ec, tds }) {
  if (Number.isFinite(ec)) {
    return Math.round(ec * 100) / 100;
  }
  if (Number.isFinite(tds)) {
    return Math.round(tds * 100) / 100;
  }
  return null;
}

function applyHydroRawColumns(hydroRow, { ph, temperature, tds, ec }) {
  if (Number.isFinite(ph)) {
    hydroRow.ph_raw = ph;
    const clamped = clampPhDisplay(ph);
    if (clamped != null) {
      hydroRow.ph_display_clamped = clamped;
      hydroRow.ph = clamped;
    }
  }
  if (Number.isFinite(temperature)) {
    hydroRow.temperature_raw = temperature;
    hydroRow.temperature = temperature;
  }
  const ecUsCm = resolveEcUsCmFromPayload({ ec, tds });
  if (ecUsCm != null) {
    hydroRow.ec = ecUsCm;
  }
}

/** Limita a columnas numeric(p,s) de Supabase (evita overflow en banco dev) */
function clampDbNumeric(value, maxAbs, decimals = 3) {
  if (!Number.isFinite(value)) return 0;
  let n = value;
  if (n > maxAbs) n = maxAbs;
  if (n < -maxAbs) n = -maxAbs;
  const factor = 10 ** decimals;
  return Math.round(n * factor) / factor;
}

function validateTelemetry(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const waterLevelOk = payload.water_level_ok;
  if (typeof waterLevelOk !== 'boolean') {
    return { ok: false, reason: 'water_level_ok must be boolean' };
  }

  const hasTemp = payload.temperature != null;
  const hasPh = payload.ph != null;
  const temperature = hasTemp ? Number(payload.temperature) : null;
  const ph = hasPh ? Number(payload.ph) : null;

  const legacyTdsRaw = payload.tds != null ? Number(payload.tds) : null;
  const ecPayload = payload.ec != null ? Number(payload.ec) : null;

  if (hasTemp && !Number.isFinite(temperature)) {
    return { ok: false, reason: 'temperature must be a finite number when present' };
  }
  if (hasPh && !Number.isFinite(ph)) {
    return { ok: false, reason: 'ph must be a finite number when present' };
  }
  if (payload.ec != null && !Number.isFinite(ecPayload)) {
    return { ok: false, reason: 'ec must be a finite number when present' };
  }
  if (payload.tds != null && !Number.isFinite(legacyTdsRaw)) {
    return { ok: false, reason: 'tds must be a finite number when present (legacy alias for ec µS/cm)' };
  }

  const resolvedEc = resolveEcUsCmFromPayload({
    ec: payload.ec != null ? ecPayload : null,
    tds: payload.tds != null ? legacyTdsRaw : null,
  });
  const hasEc = resolvedEc != null;

  const hydroRow = {
    device_id: deviceId,
    water_level_ok: waterLevelOk,
    ...(typeof payload.level_1 === 'boolean' ? { level_1: payload.level_1 } : {}),
    ...(typeof payload.level_2 === 'boolean' ? { level_2: payload.level_2 } : {}),
    ...(typeof payload.level_3 === 'boolean' ? { level_3: payload.level_3 } : {}),
    ...(typeof payload.level_4 === 'boolean' ? { level_4: payload.level_4 } : {}),
    ...(normalizeWaterLevel(payload.water_level)
      ? { water_level: normalizeWaterLevel(payload.water_level) }
      : {}),
    ...(typeof payload.levels_simulated === 'boolean'
      ? { levels_simulated: payload.levels_simulated }
      : {}),
  };

  applyHydroRawColumns(hydroRow, {
    ph: hasPh ? ph : null,
    temperature: hasTemp ? temperature : null,
    tds: payload.tds != null ? legacyTdsRaw : null,
    ec: payload.ec != null ? ecPayload : null,
  });

  const airTempRaw =
    payload.air_temp != null ? Number(payload.air_temp) : hasTemp ? temperature : NaN;
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
    hydroRow,
    sensorFields: { hasPh, hasTemp, hasEc },
    deviceStatusPatch: buildLevelDeviceStatusPatch(payload),
    envRow,
    envSkip,
    envReceived:
      payload.air_temp != null || payload.humidity != null
        ? { airTemp: airTempRaw, humidity: humidityRaw }
        : null,
  };
}

function buildLevelDeviceStatusPatch(payload) {
  const patch = {};
  if (typeof payload.level_1 === 'boolean') patch.level_1 = payload.level_1;
  if (typeof payload.level_2 === 'boolean') patch.level_2 = payload.level_2;
  if (typeof payload.level_3 === 'boolean') patch.level_3 = payload.level_3;
  if (typeof payload.level_4 === 'boolean') patch.level_4 = payload.level_4;
  const wl = normalizeWaterLevel(payload.water_level);
  if (wl) patch.water_level = wl;
  if (typeof payload.water_level_ok === 'boolean') {
    patch.water_level_ok = payload.water_level_ok;
  }
  if (typeof payload.levels_simulated === 'boolean') {
    patch.levels_simulated = payload.levels_simulated;
  }
  const im = normalizeInterlockMode(payload.interlock_mode);
  if (im) patch.level_interlock_mode = im;
  return Object.keys(patch).length > 0 ? patch : null;
}

async function patchDeviceLevel(deviceId, patch) {
  if (!patch || Object.keys(patch).length === 0) return true;

  const nowIso = new Date().toISOString();
  const { error } = await supabase
    .from('device_status')
    .update({ ...patch, last_seen: nowIso, updated_at: nowIso })
    .eq('device_id', deviceId);

  if (error) {
    console.error(`[bridge] device_status level patch failed (${deviceId}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] PATCH device_status ${deviceId} water_level=${patch.water_level ?? '-'}`
  );
  return true;
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
      ...(Number.isFinite(Number(payload.dilution_target_l))
        ? { ec_dilution_target_l: Number(payload.dilution_target_l) }
        : {}),
      ...(Number.isFinite(Number(payload.dilution_progress_l))
        ? { ec_dilution_progress_l: Number(payload.dilution_progress_l) }
        : {}),
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

function validateEcMetric(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const ecSetpoint = Number(payload.ec_setpoint);
  const ecActual = Number(payload.ec_actual);
  const ecError = Number(payload.ec_error);
  const dosageMl = Number(payload.dosage_ml ?? 0);
  const dosageTimeSeconds = Number(payload.dosage_time_seconds ?? 0);

  if (!Number.isFinite(ecSetpoint) || !Number.isFinite(ecActual) || !Number.isFinite(ecError)) {
    return { ok: false, reason: 'ec_setpoint/ec_actual/ec_error must be numbers' };
  }
  if (!Number.isFinite(dosageMl) || dosageMl < 0) {
    return { ok: false, reason: 'dosage_ml must be >= 0' };
  }
  if (!Number.isFinite(dosageTimeSeconds) || dosageTimeSeconds < 0) {
    return { ok: false, reason: 'dosage_time_seconds must be >= 0' };
  }

  const row = {
    device_id: deviceId,
    ec_setpoint: Math.round(ecSetpoint * 100) / 100,
    ec_actual: Math.round(ecActual * 100) / 100,
    ec_error: Math.round(ecError * 100) / 100,
    dosage_ml: Math.round(dosageMl * 1000) / 1000,
    dosage_time_seconds: Math.round(dosageTimeSeconds * 100) / 100,
    auto_enabled: Boolean(payload.auto_enabled),
    adjustment_needed: Boolean(payload.adjustment_needed),
    adjustment_applied: Boolean(payload.adjustment_applied),
  };

  if (payload.k_value != null && Number.isFinite(Number(payload.k_value))) {
    row.k_value = Number(payload.k_value);
  }
  if (payload.base_dose != null && Number.isFinite(Number(payload.base_dose))) {
    row.base_dose = Number(payload.base_dose);
  }
  if (payload.flow_rate != null && Number.isFinite(Number(payload.flow_rate))) {
    row.flow_rate = Number(payload.flow_rate);
  }
  if (payload.volume != null && Number.isFinite(Number(payload.volume))) {
    row.volume = Number(payload.volume);
  }
  if (payload.total_ml != null && Number.isFinite(Number(payload.total_ml))) {
    row.total_ml = Number(payload.total_ml);
  }
  if (payload.kp != null && Number.isFinite(Number(payload.kp))) {
    row.kp = Number(payload.kp);
  }
  if (payload.sequence_id) {
    row.sequence_id = String(payload.sequence_id).trim();
  }

  return { ok: true, row };
}

function validatePhMetric(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const phSetpoint = Number(payload.ph_setpoint);
  const phBeforeRaw = Number(payload.ph_before);
  let errorH = Number(payload.error_h ?? 0);
  const doseRealMl = Number(payload.dose_real_ml ?? 0);
  const doseIdealMl = Number(payload.dose_ideal_ml ?? 0);
  const dosageTimeSeconds = Number(payload.dosage_time_seconds ?? 0);

  if (!Number.isFinite(phSetpoint) || !Number.isFinite(phBeforeRaw)) {
    return { ok: false, reason: 'ph_setpoint/ph_before must be numbers' };
  }
  const phBefore = clampPhMetricColumn(phBeforeRaw);
  const phSetpointDb = clampPhMetricColumn(phSetpoint);
  errorH = sanitizeErrorH(phBeforeRaw, phSetpoint, errorH);
  if (phBeforeRaw !== phBefore) {
    console.log(
      `[bridge] ph_metric ${deviceId} ph_before clamped ${phBeforeRaw} â†’ ${phBefore} (numeric(8,3))`
    );
  }
  if (!Number.isFinite(doseRealMl) || doseRealMl < 0) {
    return { ok: false, reason: 'dose_real_ml must be >= 0' };
  }

  const doseIdealDb = clampDbNumeric(doseIdealMl, 9999999.999, 3);
  const doseRealDb = clampDbNumeric(doseRealMl, 9999999.999, 3);
  const dosageTimeDb = clampDbNumeric(dosageTimeSeconds, 99999999.99, 2);
  if (doseIdealMl !== doseIdealDb || doseRealMl !== doseRealDb) {
    console.log(
      `[bridge] ph_metric ${deviceId} dose clamped ideal=${doseIdealMl}â†’${doseIdealDb} real=${doseRealMl}â†’${doseRealDb}`
    );
  }

  const row = {
    device_id: deviceId,
    ph_setpoint: clampDbNumeric(phSetpointDb, 999.999, 3),
    ph_before: phBefore,
    error_h: errorH,
    dose_ideal_ml: doseIdealDb,
    dose_real_ml: doseRealDb,
    dosage_time_seconds: dosageTimeDb,
    auto_enabled: Boolean(payload.auto_enabled),
    adjustment_needed: Boolean(payload.adjustment_needed),
    adjustment_applied: Boolean(payload.adjustment_applied),
  };

  if (payload.direction && PH_DOSE_DIRECTIONS.has(String(payload.direction))) {
    row.direction = String(payload.direction);
  }
  if (payload.k_acid != null && Number.isFinite(Number(payload.k_acid))) {
    row.k_acid = Number(payload.k_acid);
  }
  if (payload.k_base != null && Number.isFinite(Number(payload.k_base))) {
    row.k_base = Number(payload.k_base);
  }
  if (payload.k_used != null && Number.isFinite(Number(payload.k_used))) {
    row.k_used = Number(payload.k_used);
  }
  if (payload.aggressiveness != null && Number.isFinite(Number(payload.aggressiveness))) {
    row.aggressiveness = clampDbNumeric(Number(payload.aggressiveness), 999.999, 3);
  }
  if (payload.sequence_id) {
    row.sequence_id = String(payload.sequence_id).trim();
  }

  return { ok: true, row };
}

function validatePhOperation(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const state = String(payload.ph_operation_state || '');
  if (!PH_OPERATION_STATES.has(state)) {
    return { ok: false, reason: `invalid ph_operation_state: ${state}` };
  }

  const remaining = Number(payload.ph_operation_remaining_sec);
  const nextCheck = Number(payload.ph_next_check_in_sec);
  if (!Number.isFinite(remaining) || remaining < 0) {
    return { ok: false, reason: 'ph_operation_remaining_sec must be >= 0' };
  }
  if (!Number.isFinite(nextCheck) || nextCheck < 0) {
    return { ok: false, reason: 'ph_next_check_in_sec must be >= 0' };
  }

  return {
    ok: true,
    row: {
      device_id: deviceId,
      ph_operation_state: state,
      ph_operation_remaining_sec: Math.floor(remaining),
      ph_next_check_in_sec: Math.floor(nextCheck),
    },
  };
}

function validatePhDose(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const sequenceId = String(payload.sequence_id || '').trim();
  const direction = String(payload.direction || '');
  const relayNumber = Number(payload.relay_number);
  const dosageMl = Number(payload.dosage_ml);
  const dosageTimeSeconds = Number(payload.dosage_time_seconds);
  const source = String(payload.source || 'auto_ph');

  if (!sequenceId) {
    return { ok: false, reason: 'sequence_id required' };
  }
  if (!PH_DOSE_DIRECTIONS.has(direction)) {
    return { ok: false, reason: `invalid direction: ${direction}` };
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
  if (!PH_DOSE_SOURCES.has(source)) {
    return { ok: false, reason: `invalid source: ${source}` };
  }

  const row = {
    device_id: deviceId,
    sequence_id: sequenceId,
    direction,
    relay_number: relayNumber,
    dosage_ml: Math.round(dosageMl * 1000) / 1000,
    dosage_time_seconds: Math.round(dosageTimeSeconds * 100) / 100,
    source,
  };

  if (payload.ph_before != null && Number.isFinite(Number(payload.ph_before))) {
    row.ph_before = Math.round(Number(payload.ph_before) * 100) / 100;
  }
  if (payload.ph_setpoint != null && Number.isFinite(Number(payload.ph_setpoint))) {
    row.ph_setpoint = Math.round(Number(payload.ph_setpoint) * 100) / 100;
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
  // Volumen A→B: no ahogar progreso de dilución (litros de sesión).
  const prevProgress = Number(prev.progressL);
  const nextProgress = Number(row.ec_dilution_progress_l);
  if (
    Number.isFinite(nextProgress) &&
    (!Number.isFinite(prevProgress) || Math.abs(nextProgress - prevProgress) >= 0.05)
  ) {
    return false;
  }
  const prevTarget = Number(prev.targetL);
  const nextTarget = Number(row.ec_dilution_target_l);
  if (
    Number.isFinite(nextTarget) &&
    (!Number.isFinite(prevTarget) || Math.abs(nextTarget - prevTarget) >= 0.01)
  ) {
    return false;
  }
  return now - prev.at < ecOperationThrottleMs;
}

function rememberEcOperation(deviceId, row) {
  lastEcOperationSnapshotByDevice.set(deviceId, {
    state: row.ec_operation_state,
    remaining: row.ec_operation_remaining_sec,
    progressL: row.ec_dilution_progress_l,
    targetL: row.ec_dilution_target_l,
    at: Date.now(),
  });
}

function shouldThrottlePhOperation(deviceId, row) {
  const prev = lastPhOperationSnapshotByDevice.get(deviceId);
  const now = Date.now();
  if (!prev) {
    return false;
  }
  if (prev.state !== row.ph_operation_state) {
    return false;
  }
  if (Math.abs(prev.remaining - row.ph_operation_remaining_sec) > 2) {
    return false;
  }
  return now - prev.at < phOperationThrottleMs;
}

function rememberPhOperation(deviceId, row) {
  lastPhOperationSnapshotByDevice.set(deviceId, {
    state: row.ph_operation_state,
    remaining: row.ph_operation_remaining_sec,
    at: Date.now(),
  });
}

const HYDRO_INSERT_COLUMNS = new Set([
  'device_id',
  'water_level_ok',
  'level_1',
  'level_2',
  'level_3',
  'level_4',
  'water_level',
  'temperature',
  'temperature_raw',
  'ph',
  'ph_raw',
  'ph_display_clamped',
  'ec',
  'levels_simulated',
]);

function pickHydroInsertRow(row) {
  const out = {};
  for (const key of HYDRO_INSERT_COLUMNS) {
    const value = row[key];
    if (value !== undefined && value !== null) {
      out[key] = value;
    }
  }
  return out;
}

function hasHydroRowPayload(hydroRow, sensorFields) {
  if (!hydroRow) return false;
  if (typeof hydroRow.water_level_ok === 'boolean') return true;
  return hasHydroSensorPayload(hydroRow, sensorFields);
}

function hasHydroSensorPayload(hydroRow, sensorFields) {
  if (sensorFields) {
    return !!(sensorFields.hasPh || sensorFields.hasTemp || sensorFields.hasEc);
  }
  if (!hydroRow) return false;
  const finiteNonZero = (v) => {
    if (v == null) return false;
    const n = Number(v);
    return Number.isFinite(n) && n !== 0;
  };
  return (
    finiteNonZero(hydroRow.ph_raw) ||
    finiteNonZero(hydroRow.temperature_raw) ||
    finiteNonZero(hydroRow.ec) ||
    finiteNonZero(hydroRow.temperature)
  );
}

/** Legacy NOT NULL en Supabase â€” solo rellena columnas que faltan cuando hay PV real */
function applyLegacyHydroNotNullDefaults(row, sensorFields) {
  const out = { ...row };
  if (out.ph == null && out.ph_display_clamped != null) {
    out.ph = out.ph_display_clamped;
  } else if (out.ph == null && out.ph_raw != null) {
    const clamped = clampPhDisplay(out.ph_raw);
    if (clamped != null) out.ph = clamped;
  }
  return out;
}

async function insertHydro(row, sensorFields) {
  const payload = pickHydroInsertRow(applyLegacyHydroNotNullDefaults(row, sensorFields));
  const { error } = await supabase.from('hydro_measurements').insert(payload);
  if (error) {
    console.error(`[bridge] Supabase insert failed (${payload.device_id}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] INSERT hydro_measurements ${payload.device_id} ph_raw=${payload.ph_raw ?? '-'} ph_disp=${payload.ph_display_clamped ?? payload.ph ?? '-'} ec=${payload.ec ?? '-'}`
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

async function ensureRelayMasterRow(deviceId) {
  const { data: existing, error: readErr } = await supabase
    .from('relay_master')
    .select('device_id')
    .eq('device_id', deviceId)
    .maybeSingle();

  if (readErr) {
    console.error(`[bridge] relay_master lookup failed (${deviceId}):`, readErr.message);
    return false;
  }
  if (existing) return true;

  const { data: ds, error: dsErr } = await supabase
    .from('device_status')
    .select('device_id, user_email, mac_address')
    .eq('device_id', deviceId)
    .maybeSingle();

  if (dsErr) {
    console.error(`[bridge] device_status lookup failed (${deviceId}):`, dsErr.message);
    return false;
  }
  if (!ds) {
    console.warn(
      `[bridge] relay_master row missing â€” device_status not found for ${deviceId} (register device first)`
    );
    return false;
  }

  const { error: insertErr } = await supabase.from('relay_master').insert({
    device_id: ds.device_id,
    user_email: ds.user_email,
    master_mac_address: ds.mac_address || 'unknown',
  });

  if (insertErr) {
    console.error(`[bridge] relay_master seed insert failed (${deviceId}):`, insertErr.message);
    return false;
  }

  console.log(`[bridge] INSERT relay_master seeded from device_status ${deviceId}`);
  return true;
}

async function patchRelayMasterOperation(deviceId, patch, logLabel, retryAfterSeed = true) {
  const { data, error } = await supabase
    .from('relay_master')
    .update(patch)
    .eq('device_id', deviceId)
    .select('device_id')
    .maybeSingle();

  if (error) {
    console.error(`[bridge] relay_master ${logLabel} patch failed (${deviceId}):`, error.message);
    return false;
  }

  if (!data) {
    if (retryAfterSeed && (await ensureRelayMasterRow(deviceId))) {
      return patchRelayMasterOperation(deviceId, patch, logLabel, false);
    }
    console.warn(`[bridge] relay_master ${logLabel} PATCH matched 0 rows (${deviceId})`);
    return false;
  }

  return true;
}

async function insertFlowSessionReading(row) {
  if (row.ec_dilution_progress_l == null && row.ec_dilution_target_l == null) {
    return true;
  }
  const isDiluting =
    row.ec_operation_state === 'diluting_draining' ||
    row.ec_operation_state === 'diluting_filling';
  if (!isDiluting && row.ec_dilution_progress_l == null) {
    return true;
  }

  const payload = {
    device_id: row.device_id,
    sensor_id: 0,
    role: 'dilution',
    phase: isDiluting ? row.ec_operation_state : null,
    session_liters: Number.isFinite(Number(row.ec_dilution_progress_l))
      ? Number(row.ec_dilution_progress_l)
      : 0,
    target_liters: Number.isFinite(Number(row.ec_dilution_target_l))
      ? Number(row.ec_dilution_target_l)
      : null,
    active: isDiluting,
  };

  const { error } = await supabase.from('hydro_flow_readings').insert(payload);
  if (error) {
    // Tabla puede no existir aún — no bloquear ec_operation.
    console.warn(
      `[bridge] hydro_flow_readings insert skipped (${row.device_id}):`,
      error.message
    );
    return false;
  }
  return true;
}

async function patchEcOperation(row) {
  const patch = {
    ec_operation_state: row.ec_operation_state,
    ec_operation_remaining_sec: row.ec_operation_remaining_sec,
    ec_next_check_in_sec: row.ec_next_check_in_sec,
  };
  if (row.ec_dilution_target_l != null) {
    patch.ec_dilution_target_l = row.ec_dilution_target_l;
  }
  if (row.ec_dilution_progress_l != null) {
    patch.ec_dilution_progress_l = row.ec_dilution_progress_l;
  }

  const ok = await patchRelayMasterOperation(row.device_id, patch, 'ec_operation');
  if (!ok) return false;

  // Historial volumen A→B (no bloquea si falla).
  await insertFlowSessionReading(row);

  console.log(
    `[bridge] PATCH relay_master ${row.device_id} ec_operation=${row.ec_operation_state} rem=${row.ec_operation_remaining_sec}s`
  );
  return true;
}

async function patchPhOperation(row) {
  const patch = {
    ph_operation_state: row.ph_operation_state,
    ph_operation_remaining_sec: row.ph_operation_remaining_sec,
    ph_next_check_in_sec: row.ph_next_check_in_sec,
  };

  const ok = await patchRelayMasterOperation(row.device_id, patch, 'ph_operation');
  if (!ok) return false;

  console.log(
    `[bridge] PATCH relay_master ${row.device_id} ph_operation=${row.ph_operation_state} rem=${row.ph_operation_remaining_sec}s next=${row.ph_next_check_in_sec}s`
  );
  return true;
}

async function incrementPumpQuantity({ deviceId, relayIndex, ml, sequenceId, role }) {
  const dosageMl = Number(ml);
  if (!Number.isFinite(dosageMl) || dosageMl <= 0) return true;
  if (!sequenceId) return true;
  if (!Number.isInteger(relayIndex) || relayIndex < 0 || relayIndex > 7) return true;

  const { error } = await supabase.rpc('increment_pump_quantity', {
    p_device_id: deviceId,
    p_relay_index: relayIndex,
    p_ml: dosageMl,
    p_sequence_id: String(sequenceId),
    p_role: role || 'other',
  });
  if (error) {
    console.error(
      `[bridge] increment_pump_quantity failed (${deviceId} r${relayIndex}):`,
      error.message
    );
    return false;
  }
  console.log(
    `[bridge] pump_quantity +${dosageMl}ml ${deviceId} r${relayIndex} seq=${sequenceId}`
  );
  return true;
}

async function insertDose(row) {
  const { error } = await supabase.from('nutrient_dosages').upsert(row, {
    onConflict: 'device_id,sequence_id,nutrient_name,relay_number',
    ignoreDuplicates: true,
  });
  if (error) {
    console.error(`[bridge] nutrient_dosages insert failed (${row.device_id}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] INSERT nutrient_dosages ${row.device_id} ${row.nutrient_name} ${row.dosage_ml}ml seq=${row.sequence_id}`
  );
  await incrementPumpQuantity({
    deviceId: row.device_id,
    relayIndex: Number(row.relay_number),
    ml: row.dosage_ml,
    sequenceId: row.sequence_id,
    role: 'ec',
  });
  return true;
}

async function insertPhDose(row) {
  const { error } = await supabase.from('ph_dosages').upsert(row, {
    onConflict: 'device_id,sequence_id,direction,relay_number',
    ignoreDuplicates: true,
  });
  if (error) {
    console.error(`[bridge] ph_dosages insert failed (${row.device_id}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] INSERT ph_dosages ${row.device_id} ${row.direction} ${row.dosage_ml}ml seq=${row.sequence_id}`
  );
  const dir = String(row.direction || '').toLowerCase();
  const role = dir === 'up' || dir === 'ph_up' ? 'ph_up' : dir === 'down' || dir === 'ph_down' ? 'ph_down' : 'other';
  await incrementPumpQuantity({
    deviceId: row.device_id,
    relayIndex: Number(row.relay_number),
    ml: row.dosage_ml,
    sequenceId: row.sequence_id,
    role,
  });
  return true;
}

async function insertEcMetric(row) {
  const { error } = await supabase.from('ec_controller_metrics').insert(row);
  if (error) {
    console.error(`[bridge] ec_controller_metrics insert failed (${row.device_id}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] INSERT ec_controller_metrics ${row.device_id} err=${row.ec_error} u(t)=${row.dosage_ml}ml`
  );
  return true;
}

async function insertPhMetric(row) {
  const { error } = await supabase.from('ph_controller_metrics').insert(row);
  if (error) {
    console.error(
      `[bridge] ph_controller_metrics insert failed (${row.device_id}):`,
      error.message,
      JSON.stringify(row)
    );
    return false;
  }
  console.log(
    `[bridge] INSERT ph_controller_metrics ${row.device_id} u(t)=${row.dose_real_ml}ml`
  );
  return true;
}

async function patchOnline(deviceId, online) {
  const patch = online
    ? { is_online: true, last_seen: new Date().toISOString() }
    : { is_online: false };

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

  if (hasHydroRowPayload(validated.hydroRow, validated.sensorFields)) {
    await insertHydro(validated.hydroRow, validated.sensorFields);
  } else {
    console.log(`[bridge] telemetry ${deviceId} — skip hydro (sem niveles/sensores)`);
  }
  if (validated.deviceStatusPatch) {
    await patchDeviceLevel(deviceId, validated.deviceStatusPatch);
  }
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

/** Evento on-change L1–L4 — solo PATCH device_status (sin throttle 30s de hydro). */
async function handleLevels(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'levels');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  if (payload.device_id && String(payload.device_id) !== deviceId) {
    console.warn(`[bridge] Rejected ${topic}: device_id mismatch`);
    return;
  }

  const patch = buildLevelDeviceStatusPatch(payload);
  if (!patch) {
    console.warn(`[bridge] Rejected ${topic}: no level fields`);
    return;
  }

  if (typeof payload.water_level_ok !== 'boolean') {
    console.warn(`[bridge] Rejected ${topic}: water_level_ok must be boolean`);
    return;
  }

  if (shouldThrottle(lastLevelsEventByDevice, deviceId, levelsEventThrottleMs)) {
    console.log(
      `[bridge] Throttled levels ${deviceId} (< ${levelsEventThrottleMs}ms anti-flood)`
    );
    return;
  }

  await patchDeviceLevel(deviceId, patch);
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
    lastHeartbeatAtByDevice.set(deviceId, Date.now());
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

function validateEcDilution(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const sequenceId = String(payload.sequence_id || '').trim();
  const source = String(payload.source || 'auto');
  const volumeTarget = Number(payload.volume_target_l);
  const volumeMeasured = Number(payload.volume_measured_l);

  if (!sequenceId) {
    return { ok: false, reason: 'sequence_id required' };
  }
  if (!EC_DILUTION_SOURCES.has(source)) {
    return { ok: false, reason: `invalid source: ${source}` };
  }
  if (!Number.isFinite(volumeTarget) || volumeTarget < 0) {
    return { ok: false, reason: 'volume_target_l invalid' };
  }
  if (!Number.isFinite(volumeMeasured) || volumeMeasured < 0) {
    return { ok: false, reason: 'volume_measured_l invalid' };
  }

  return {
    ok: true,
    row: {
      device_id: deviceId,
      sequence_id: sequenceId,
      source,
      ec_before: payload.ec_before != null ? Number(payload.ec_before) : null,
      ec_setpoint: payload.ec_setpoint != null ? Number(payload.ec_setpoint) : null,
      volume_target_l: Math.round(volumeTarget * 1000) / 1000,
      volume_measured_l: Math.round(volumeMeasured * 1000) / 1000,
      drain_duration_s:
        payload.drain_duration_s != null ? Number(payload.drain_duration_s) : null,
      fill_duration_s:
        payload.fill_duration_s != null ? Number(payload.fill_duration_s) : null,
    },
  };
}

async function insertEcDilution(row) {
  const payload = {
    device_id: row.device_id,
    sequence_id: row.sequence_id,
    source: row.source,
    volume_target_l: row.volume_target_l,
    volume_measured_l: row.volume_measured_l,
  };
  if (row.ec_before != null && Number.isFinite(row.ec_before)) {
    payload.ec_before = row.ec_before;
  }
  if (row.ec_setpoint != null && Number.isFinite(row.ec_setpoint)) {
    payload.ec_setpoint = row.ec_setpoint;
  }
  if (row.drain_duration_s != null && Number.isFinite(row.drain_duration_s)) {
    payload.drain_duration_s = row.drain_duration_s;
  }
  if (row.fill_duration_s != null && Number.isFinite(row.fill_duration_s)) {
    payload.fill_duration_s = row.fill_duration_s;
  }

  const { error } = await supabase.from('ec_dilution_events').insert(payload);
  if (error) {
    console.error(`[bridge] ec_dilution_events insert failed (${row.device_id}):`, error.message);
    return false;
  }
  console.log(
    `[bridge] INSERT ec_dilution_events ${row.device_id} vol=${row.volume_measured_l}L src=${row.source}`
  );
  return true;
}

async function handleEcDilution(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'ec_dilution');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateEcDilution(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  await insertEcDilution(validated.row);
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

async function handlePhOperation(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'ph_operation');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validatePhOperation(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  if (shouldThrottlePhOperation(deviceId, validated.row)) {
    console.log(`[bridge] Throttled ph_operation ${deviceId} (redundant)`);
    return;
  }

  const ok = await patchPhOperation(validated.row);
  if (ok) {
    rememberPhOperation(deviceId, validated.row);
  }
}

async function handlePhDose(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'ph_dose');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validatePhDose(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  await insertPhDose(validated.row);
}

async function handleEcMetric(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'ec_metric');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateEcMetric(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  await insertEcMetric(validated.row);
}

async function handlePhMetric(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'ph_metric');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validatePhMetric(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  await insertPhMetric(validated.row);
}

function slaveDeviceIdFromMac(mac) {
  return `ESP32_SLAVE_${String(mac).replace(/:/g, '_')}`;
}

function validateCommandAck(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const commandId = Number(payload.id);
  const relayIndex = Number(payload.relay_index);
  const status = String(payload.status || 'completed');
  if (!Number.isInteger(commandId) || commandId <= 0) {
    return { ok: false, reason: 'id must be positive integer (relay_commands.id)' };
  }
  if (!COMMAND_ACK_STATUSES.has(status)) {
    return { ok: false, reason: `status must be one of ${[...COMMAND_ACK_STATUSES].join(', ')}` };
  }
  if (!Number.isInteger(relayIndex) || relayIndex < 0 || relayIndex > 15) {
    return { ok: false, reason: 'relay_index out of range' };
  }
  if (typeof payload.current_state !== 'boolean') {
    return { ok: false, reason: 'current_state must be boolean' };
  }

  let slaveMac = null;
  let relayStates = null;
  const macRaw = payload.slave_mac_address || payload.target_device_id;
  if (macRaw) {
    slaveMac = String(macRaw).toUpperCase();
    if (!SLAVE_MAC_RE.test(slaveMac)) {
      return { ok: false, reason: `invalid slave_mac_address: ${slaveMac}` };
    }
    if (Array.isArray(payload.relay_states)) {
      relayStates = payload.relay_states.map((v) => Boolean(v));
      if (relayStates.length < 1 || relayStates.length > 8) {
        return { ok: false, reason: 'relay_states must have 1-8 booleans' };
      }
    }
  }

  return {
    ok: true,
    row: {
      deviceId,
      commandId,
      status,
      relayIndex,
      currentState: payload.current_state,
      slaveMac,
      relayStates,
    },
  };
}

async function rpcCompleteRelayCommand(row) {
  const dedupeKey = `${row.deviceId}:${row.commandId}`;
  const lastAt = completedCommandAckIds.get(dedupeKey);
  if (lastAt && Date.now() - lastAt < 5000) {
    console.log(`[bridge] command_ack dedup ${dedupeKey}`);
    return true;
  }

  if (row.status === 'failed') {
    const { error } = await supabase
      .from('relay_commands')
      .update({
        status: 'failed',
        error_message: 'device reported failed',
        completed_at: new Date().toISOString(),
      })
      .eq('id', row.commandId)
      .eq('device_id', row.deviceId)
      .in('status', ['pending', 'sent', 'processing']);
    if (error) {
      console.error(`[bridge] command_ack FAILED id=${row.commandId}:`, error.message);
      return false;
    }
    completedCommandAckIds.set(dedupeKey, Date.now());
    console.log(
      `[bridge] command_ack FAILED id=${row.commandId} relay=${row.relayIndex} state=${row.currentState}`
    );
    return true;
  }

  const args = {
    p_command_id: row.commandId,
    p_device_id: row.deviceId,
    p_current_state: row.currentState,
  };
  if (row.slaveMac && row.relayStates) {
    args.p_slave_mac = row.slaveMac;
    args.p_relay_states = row.relayStates;
  }

  const { data, error } = await supabase.rpc('complete_relay_command', args);
  if (error) {
    console.error(`[bridge] complete_relay_command id=${row.commandId}:`, error.message);
    return false;
  }

  if (!data || data.length === 0) {
    const { data: existing, error: selErr } = await supabase
      .from('relay_commands')
      .select('id, status')
      .eq('id', row.commandId)
      .eq('device_id', row.deviceId)
      .maybeSingle();
    if (selErr) {
      console.error(`[bridge] command_ack verify id=${row.commandId}:`, selErr.message);
      return false;
    }
    if (existing?.status === 'completed') {
      completedCommandAckIds.set(dedupeKey, Date.now());
      console.log(`[bridge] command_ack id=${row.commandId} already completed`);
      return true;
    }
    console.warn(`[bridge] command_ack id=${row.commandId} RPC returned 0 rows`);
    return false;
  }

  completedCommandAckIds.set(dedupeKey, Date.now());
  console.log(
    `[bridge] RPC complete_relay_command id=${row.commandId} relay=${row.relayIndex} state=${row.currentState}`
  );
  return true;
}

function validateRelayState(deviceId, payload) {
  if (!isValidDeviceId(deviceId)) {
    return { ok: false, reason: 'invalid device_id format' };
  }
  const idCheck = checkDeviceIdMatch(deviceId, payload);
  if (!idCheck.ok) return idCheck;

  const hasMaster = Array.isArray(payload.master) && payload.master.length > 0;
  const slaveMacRaw = payload.slave_mac_address;
  const isHeartbeat = payload.heartbeat === true;
  const hasRelayStates =
    Array.isArray(payload.relay_states) && payload.relay_states.length > 0;
  const hasSlaveLink = Boolean(slaveMacRaw) && (hasRelayStates || isHeartbeat);

  if (!hasMaster && !hasSlaveLink) {
    return { ok: false, reason: 'need master[] or slave_mac_address + (relay_states[] or heartbeat)' };
  }

  let slaveMac = null;
  let relayStates = null;
  let relayHasTimers = null;
  let relayRemainingTimes = null;

  if (slaveMacRaw) {
    slaveMac = String(slaveMacRaw).toUpperCase();
    if (!SLAVE_MAC_RE.test(slaveMac)) {
      return { ok: false, reason: `invalid slave_mac_address: ${slaveMac}` };
    }
    if (hasRelayStates) {
      relayStates = payload.relay_states.map((v) => Boolean(v));
      if (relayStates.length > 8) {
        return { ok: false, reason: 'relay_states max 8' };
      }
      if (Array.isArray(payload.relay_has_timers)) {
        relayHasTimers = payload.relay_has_timers.map((v) => Boolean(v));
      }
      if (Array.isArray(payload.relay_remaining_times)) {
        relayRemainingTimes = payload.relay_remaining_times.map((v) => Number(v) || 0);
      }
    }
  }

  return {
    ok: true,
    row: {
      deviceId,
      master: hasMaster ? payload.master.map((v) => Boolean(v)) : null,
      slaveMac,
      relayStates,
      relayHasTimers,
      relayRemainingTimes,
      linkOnline: typeof payload.link_online === 'boolean' ? payload.link_online : null,
      heartbeat: payload.heartbeat === true,
    },
  };
}

async function fetchDeviceUserEmail(deviceId) {
  const { data, error } = await supabase
    .from('device_status')
    .select('user_email, mac_address')
    .eq('device_id', deviceId)
    .maybeSingle();
  if (error) {
    console.error(`[bridge] device_status user_email ${deviceId}:`, error.message);
    return null;
  }
  return data;
}

async function patchRelaySlaveFromMqtt(row) {
  const dev = await fetchDeviceUserEmail(row.deviceId);
  if (!dev?.user_email) {
    console.warn(`[bridge] relay/state slave skip — no user_email for ${row.deviceId}`);
    return false;
  }

  const slaveDeviceId = slaveDeviceIdFromMac(row.slaveMac);
  const nowIso = new Date().toISOString();
  const linkOnly = row.heartbeat && !row.relayStates;

  if (linkOnly) {
    // link_online=false: no refrescar last_update (UI se quedaría verde). 
    // last_update viejo + WS UPDATE → resolveSlaveOnline = offline al instante.
    const lastUpdate =
      row.linkOnline === false
        ? new Date(Date.now() - 3 * 60 * 1000).toISOString()
        : nowIso;
    const { error } = await supabase
      .from('relay_slaves')
      .update({ last_update: lastUpdate, updated_at: nowIso })
      .eq('device_id', slaveDeviceId);
    if (error) {
      console.error(`[bridge] relay_slaves link heartbeat ${slaveDeviceId}:`, error.message);
      return false;
    }
    console.log(
      `[bridge] PATCH relay_slaves link-only ${slaveDeviceId} via MQTT link_online=${row.linkOnline}`
    );
    return true;
  }

  const patch = {
    device_id: slaveDeviceId,
    user_email: dev.user_email,
    master_device_id: row.deviceId,
    master_mac_address: dev.mac_address || '',
    slave_mac_address: row.slaveMac,
    relay_states: row.relayStates,
    last_update: nowIso,
    updated_at: nowIso,
  };
  if (row.relayHasTimers) patch.relay_has_timers = row.relayHasTimers;
  if (row.relayRemainingTimes) patch.relay_remaining_times = row.relayRemainingTimes;
  // relay_names nunca incluido — UI es fuente de verdad; upsert solo estados/timers

  const { error } = await supabase.from('relay_slaves').upsert(patch, { onConflict: 'device_id' });
  if (error) {
    console.error(`[bridge] relay_slaves upsert ${slaveDeviceId}:`, error.message);
    return false;
  }
  console.log(`[bridge] PATCH relay_slaves ${slaveDeviceId} via MQTT`);
  return true;
}

function relayStatePatchKey(deviceId, slaveMac) {
  return `${deviceId}:${slaveMac || 'master'}`;
}

function mergeRelayStateRows(prev, next) {
  if (!prev) return next;
  return {
    deviceId: next.deviceId,
    slaveMac: next.slaveMac,
    master: next.master ?? prev.master,
    relayStates: next.relayStates ?? prev.relayStates,
    relayHasTimers: next.relayHasTimers ?? prev.relayHasTimers,
    relayRemainingTimes: next.relayRemainingTimes ?? prev.relayRemainingTimes,
    linkOnline: next.linkOnline ?? prev.linkOnline,
    heartbeat: next.heartbeat ?? prev.heartbeat,
  };
}

async function flushRelayStatePatch(key) {
  const entry = pendingRelayStatePatches.get(key);
  if (!entry) return;
  pendingRelayStatePatches.delete(key);
  clearTimeout(entry.timer);
  const ok = await patchRelaySlaveFromMqtt(entry.row);
  if (ok) {
    lastRelayStatePatchByDevice.set(key, Date.now());
  }
}

function scheduleRelayStatePatch(row) {
  const key = relayStatePatchKey(row.deviceId, row.slaveMac);
  const existing = pendingRelayStatePatches.get(key);
  if (existing?.timer) {
    clearTimeout(existing.timer);
  }
  const merged = mergeRelayStateRows(existing?.row, row);
  const timer = setTimeout(() => {
    flushRelayStatePatch(key).catch((err) => {
      console.error(`[bridge] relay/state flush ${key}:`, err.message || err);
    });
  }, relayStateCoalesceMs);
  pendingRelayStatePatches.set(key, { row: merged, timer });
}

function shouldThrottleRelayHeartbeat(deviceId, slaveMac) {
  const key = relayStatePatchKey(deviceId, slaveMac);
  const last = lastRelayHeartbeatPatchByDevice.get(key) || 0;
  if (Date.now() - last < relayHeartbeatThrottleMs) return true;
  lastRelayHeartbeatPatchByDevice.set(key, Date.now());
  return false;
}

async function handleCommandAck(topic, message) {
  const deviceId = parseDeviceIdFromTopic(topic, 'command_ack');
  if (!deviceId) return;

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateCommandAck(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  await rpcCompleteRelayCommand(validated.row);
}

async function handleRelayState(topic, message) {
  const parts = topic.split('/');
  if (parts.length !== 4 || parts[0] !== 'hidrowave' || parts[2] !== 'relay' || parts[3] !== 'state') {
    return;
  }
  const deviceId = parts[1];

  let payload;
  try {
    payload = JSON.parse(message.toString());
  } catch {
    console.warn(`[bridge] Invalid JSON on ${topic}`);
    return;
  }

  const validated = validateRelayState(deviceId, payload);
  if (!validated.ok) {
    console.warn(`[bridge] Rejected ${topic}: ${validated.reason}`);
    return;
  }

  if (validated.row.slaveMac) {
    const gapKey = relayStatePatchKey(deviceId, validated.row.slaveMac);
    const last = lastRelayStatePatchByDevice.get(gapKey) || 0;
    if (last > 0 && Date.now() - last > 90000) {
      console.log(
        `[bridge] slave_link_gap device=${deviceId} mac=${validated.row.slaveMac} gap_s=${Math.round((Date.now() - last) / 1000)}`
      );
    }
  }

  if (validated.row.slaveMac) {
    const linkOnly = validated.row.heartbeat && !validated.row.relayStates;
    if (linkOnly) {
      if (shouldThrottleRelayHeartbeat(deviceId, validated.row.slaveMac)) {
        console.log(`[bridge] Throttled relay/state link heartbeat ${deviceId}`);
        return;
      }
      await patchRelaySlaveFromMqtt(validated.row);
      lastRelayStatePatchByDevice.set(
        relayStatePatchKey(deviceId, validated.row.slaveMac),
        Date.now()
      );
      return;
    }
    if (validated.row.relayStates) {
      scheduleRelayStatePatch(validated.row);
    }
  }
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
      console.warn(`[bridge] Stale heartbeat ${deviceId} â€” marking offline`);
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
  subscribedAt = Date.now();
  client.subscribe(TOPICS, { qos: 0 }, (err) => {
    if (err) {
      console.error('[bridge] Subscribe failed:', err.message);
      process.exit(1);
    }
    console.log(
      `[bridge] Subscribed ${TOPICS.join(', ')} | telemetry ${telemetryThrottleMs}ms | levels ${levelsEventThrottleMs}ms | heartbeat ${heartbeatThrottleMs}ms | ec_operation ${ecOperationThrottleMs}ms | ph_operation ${phOperationThrottleMs}ms | stale ${heartbeatStaleMs}ms | retain-grace ${RETAIN_SUBSCRIBE_GRACE_MS}ms`
    );
  });
});

client.on('message', (topic, message, packet) => {
  if (packet && packet.retain && subscribedAt > 0 && Date.now() - subscribedAt < RETAIN_SUBSCRIBE_GRACE_MS) {
    console.log(`[bridge] ignore retained on subscribe ${topic}`);
    return;
  }
  const suffix = topicSuffix(topic);
  const run = async () => {
    if (suffix === 'telemetry') {
      await handleTelemetry(topic, message);
    } else if (suffix === 'levels') {
      await handleLevels(topic, message);
    } else if (suffix === 'heartbeat') {
      await handleHeartbeat(topic, message);
    } else if (suffix === 'status') {
      await handleStatus(topic, message);
    } else if (suffix === 'ec_operation') {
      await handleEcOperation(topic, message);
    } else if (suffix === 'dose') {
      await handleDose(topic, message);
    } else if (suffix === 'ph_operation') {
      await handlePhOperation(topic, message);
    } else if (suffix === 'ph_dose') {
      await handlePhDose(topic, message);
    } else if (suffix === 'ec_metric') {
      await handleEcMetric(topic, message);
    } else if (suffix === 'ph_metric') {
      await handlePhMetric(topic, message);
    } else if (suffix === 'ec_dilution') {
      await handleEcDilution(topic, message);
    } else if (suffix === 'command_ack') {
      await handleCommandAck(topic, message);
    } else if (topic.endsWith('/relay/state')) {
      await handleRelayState(topic, message);
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
