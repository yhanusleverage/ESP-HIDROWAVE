/**
 * MQTT publish helpers for scheduler (upsert rules + procedure/cmd).
 * Same credentials pattern as index.js publishRuleDisableMqtt.
 * Does not crash the bridge if publish fails — returns { ok:false }.
 */

import { createHash } from 'crypto';
import mqtt from 'mqtt';

const DEVICE_ID_RE = /^ESP32_HIDRO_[0-9A-F]{6}$/;

function safeMqttRuleId(ruleId) {
  return String(ruleId).replace(/[^A-Za-z0-9._-]/g, '_').slice(0, 96);
}

function hashRuleBody(ruleBody) {
  return createHash('sha256').update(JSON.stringify(ruleBody)).digest('hex').slice(0, 16);
}

function mqttUrlFromEnv() {
  const host = process.env.MQTT_HOST || 'localhost';
  const port = process.env.MQTT_PORT || '1883';
  return `mqtt://${host}:${port}`;
}

function publishCreds() {
  const username = process.env.MQTT_PUBLISH_USER || process.env.MQTT_USER;
  const password = process.env.MQTT_PUBLISH_PASS || process.env.MQTT_PASS;
  return { username, password };
}

/**
 * @param {import('mqtt').MqttClient | null} bridgeClient
 * @param {string} topic
 * @param {object} body
 * @param {{ retain?: boolean }} [opts]
 */
export function publishMqttJson(bridgeClient, topic, body, opts = {}) {
  const retain = Boolean(opts.retain);
  const payload = JSON.stringify(body);
  const { username, password } = publishCreds();

  const publishOnce = (client) =>
    new Promise((resolve) => {
      if (!client || !client.connected) {
        resolve({ ok: false, error: 'mqtt not connected' });
        return;
      }
      client.publish(topic, payload, { qos: 1, retain }, (err) => {
        if (err) resolve({ ok: false, error: err.message });
        else resolve({ ok: true });
      });
    });

  const useBridge =
    bridgeClient?.connected &&
    (!process.env.MQTT_PUBLISH_USER ||
      process.env.MQTT_PUBLISH_USER === process.env.MQTT_USER);

  if (useBridge) {
    return publishOnce(bridgeClient);
  }

  if (!username || !password) {
    console.warn('[scheduler-mqtt] skip — sin MQTT_USER / MQTT_PUBLISH_*');
    return Promise.resolve({ ok: false, skipped: true });
  }

  return new Promise((resolve) => {
    const pub = mqtt.connect(mqttUrlFromEnv(), {
      username,
      password,
      connectTimeout: 5000,
    });
    let settled = false;
    const finish = (result) => {
      if (settled) return;
      settled = true;
      setTimeout(() => {
        try {
          pub.end(true);
        } catch {
          /* ignore */
        }
      }, 200);
      resolve(result);
    };
    const timer = setTimeout(() => finish({ ok: false, error: 'timeout' }), 8000);
    pub.on('connect', async () => {
      try {
        const r = await publishOnce(pub);
        clearTimeout(timer);
        finish(r);
      } catch (e) {
        clearTimeout(timer);
        finish({ ok: false, error: e instanceof Error ? e.message : String(e) });
      }
    });
    pub.on('error', (err) => {
      clearTimeout(timer);
      finish({ ok: false, error: err.message });
    });
  });
}

/**
 * Upsert retained — same shape Core expects from UI sync.
 * @param {import('mqtt').MqttClient | null} bridgeClient
 */
export async function publishRuleUpsertMqtt(bridgeClient, deviceId, ruleRow) {
  if (!DEVICE_ID_RE.test(deviceId)) {
    return { ok: false, error: 'invalid device_id' };
  }
  const ruleId = ruleRow.rule_id;
  const ruleJson =
    ruleRow.rule_json && typeof ruleRow.rule_json === 'object'
      ? ruleRow.rule_json
      : {};
  const ruleBody = {
    rule_id: ruleId,
    rule_name: ruleRow.rule_name ?? ruleId,
    rule_description: ruleRow.rule_description ?? '',
    enabled: ruleRow.enabled !== false,
    priority: ruleRow.priority ?? 50,
    rule_json: ruleJson,
  };
  if (ruleJson.condition != null) ruleBody.condition = ruleJson.condition;
  if (Array.isArray(ruleJson.conditions) && ruleJson.conditions.length > 0) {
    ruleBody.conditions = ruleJson.conditions;
  }
  if (ruleJson.actions != null) ruleBody.actions = ruleJson.actions;
  if (ruleJson.fsm != null) ruleBody.fsm = ruleJson.fsm;
  if (ruleJson.script != null) ruleBody.script = ruleJson.script;
  if (ruleJson.execution_class != null) {
    ruleBody.execution_class = ruleJson.execution_class;
  }
  if (ruleJson.procedure_kind != null) {
    ruleBody.procedure_kind = ruleJson.procedure_kind;
  }

  const topic = `hidrowave/${deviceId}/rules/${safeMqttRuleId(ruleId)}`;
  const body = {
    v: 1,
    op: 'upsert',
    device_id: deviceId,
    rule_id: ruleId,
    hash: hashRuleBody(ruleBody),
    rule: ruleBody,
  };

  const r = await publishMqttJson(bridgeClient, topic, body, { retain: true });
  if (r.ok) {
    console.log(`[scheduler-mqtt] upsert retained → ${topic}`);
  } else {
    console.error(`[scheduler-mqtt] upsert failed rule=${ruleId}:`, r.error || r.skipped);
  }
  return r;
}

/**
 * procedure/cmd start|abort|rearm — not retained.
 */
export async function publishProcedureCmdMqtt(bridgeClient, deviceId, ruleId, op = 'start') {
  if (!DEVICE_ID_RE.test(deviceId)) {
    return { ok: false, error: 'invalid device_id' };
  }
  const topic = `hidrowave/${deviceId}/procedure/cmd`;
  const body = {
    v: 1,
    device_id: deviceId,
    rule_id: ruleId,
    op,
  };
  const r = await publishMqttJson(bridgeClient, topic, body, { retain: false });
  if (r.ok) {
    console.log(`[scheduler-mqtt] procedure/cmd ${op} → ${topic} rule=${ruleId}`);
  } else {
    console.error(`[scheduler-mqtt] procedure/cmd failed:`, r.error || r.skipped);
  }
  return r;
}

export function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}
