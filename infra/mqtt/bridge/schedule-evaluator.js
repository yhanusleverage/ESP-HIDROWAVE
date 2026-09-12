/**
 * Schedule Evaluator — cada ~60s (llamado desde index.js).
 *
 * - Procedure (tanque/script FSM): mismo efecto que Ativar UI
 *   (DB enabled + MQTT upsert + 400ms + procedure/cmd start)
 * - Simple (actions relay): camino clásico relay_commands + MQTT command
 * - fn_recirculacao*: skip (tipagem/Motor — no alarma)
 *
 * Fallos MQTT/DB se loguean; no lanzan hacia index (no tumba el bridge).
 */

import {
  publishProcedureCmdMqtt,
  publishRuleUpsertMqtt,
  sleep,
} from './schedule-mqtt-publish.js';

const DEVICE_ID_RE = /^ESP32_HIDRO_[0-9A-F]{6}$/;
const SCHEDULER_BY_PREFIX = 'scheduler#';
const ATIVAR_DELAY_MS = 400;

function nowInTimezone(tz) {
  const now = new Date();
  const fmt = new Intl.DateTimeFormat('en-US', {
    timeZone: tz,
    hour: 'numeric',
    minute: 'numeric',
    weekday: 'short',
    hour12: false,
  });
  const parts = Object.fromEntries(
    fmt.formatToParts(now).map((p) => [p.type, p.value])
  );
  const dayMap = { Sun: 0, Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6 };
  return {
    hour: Number(parts.hour),
    minute: Number(parts.minute),
    dayOfWeek: dayMap[parts.weekday] ?? now.getDay(),
    now,
  };
}

function parseTime(timeStr) {
  if (!timeStr) return null;
  const parts = String(timeStr).split(':');
  if (parts.length < 2) return null;
  return { hour: Number(parts[0]), minute: Number(parts[1]) };
}

export function isFnCirculationRuleId(ruleId) {
  if (!ruleId) return false;
  const id = String(ruleId);
  return (
    id === 'fn_recirculacao_continua' ||
    id === 'fn_circulation' ||
    id.toLowerCase().startsWith('fn_recircul')
  );
}

/** Heurística alineada a mqtt-rules-publish / rule-procedure-history. */
export function isProcedureRuleJson(ruleJson) {
  if (!ruleJson || typeof ruleJson !== 'object' || Array.isArray(ruleJson)) {
    return false;
  }
  const rj = ruleJson;
  if (rj.fsm && typeof rj.fsm === 'object') return true;
  const kind = String(rj.procedure_kind ?? '');
  if (
    kind === 'full_recharge' ||
    kind === 'drain_only' ||
    kind === 'fill_only' ||
    kind === 'generic'
  ) {
    return true;
  }
  if (rj.execution_class === 'procedure') return true;
  if (Array.isArray(rj.procedure_steps) && rj.procedure_steps.length > 0) {
    return true;
  }
  if (rj.procedure_canonical && typeof rj.procedure_canonical === 'object') {
    return true;
  }
  const script = rj.script;
  if (script && typeof script === 'object') {
    const instrs = script.instructions;
    if (Array.isArray(instrs)) {
      return instrs.some(
        (i) =>
          i &&
          (i.type === 'while' ||
            i.type === 'wait_level' ||
            i.type === 'wait_liters' ||
            i.type === 'recirc' ||
            i.type === 'block_auto')
      );
    }
  }
  return false;
}

function extractActions(ruleJson) {
  if (!ruleJson) return [];
  const body = ruleJson.rule_body || ruleJson;
  const actions = body.actions || ruleJson.actions || [];
  if (!Array.isArray(actions)) return [];

  return actions
    .map((a) => {
      const relayIndex = a.target_relay ?? a.relay_number ?? null;
      if (relayIndex == null) return null;

      let actionStr = 'on';
      const type = (a.type || '').toLowerCase();
      if (type === 'relay_off' || type === 'off') actionStr = 'off';
      else if (type === 'relay_pulse' || type === 'pulse' || type === 'toggle') {
        actionStr = 'on';
      }

      const durationMs = Number(a.duration_ms || 0);
      const durationS = durationMs > 0 ? Math.floor(durationMs / 1000) : 0;

      return {
        relay_index: Number(relayIndex),
        action: actionStr,
        duration_s: durationS,
        target_device_id: a.target_device_id || null,
      };
    })
    .filter(Boolean);
}

/**
 * @param {import('@supabase/supabase-js').SupabaseClient} supabase
 * @param {import('mqtt').MqttClient} mqttClient
 */
export async function evaluateSchedules(supabase, mqttClient) {
  const { data: schedules, error: schErr } = await supabase
    .from('rule_schedules')
    .select('*')
    .eq('enabled', true);

  if (schErr) {
    console.error('[scheduler] fetch rule_schedules error:', schErr.message);
    return;
  }
  if (!schedules || schedules.length === 0) return;

  /** @type {Array<{ sched: object, rule: object, now: Date }>} */
  const matches = [];

  for (const sched of schedules) {
    try {
      const hit = await matchSchedule(supabase, sched);
      if (hit) matches.push(hit);
    } catch (e) {
      console.error(
        `[scheduler] match error schedule=${sched.id}:`,
        e instanceof Error ? e.message : e
      );
    }
  }

  matches.sort(
    (a, b) => (Number(b.rule.priority) || 50) - (Number(a.rule.priority) || 50)
  );

  for (const m of matches) {
    try {
      await fireMatch(supabase, mqttClient, m);
    } catch (e) {
      console.error(
        `[scheduler] fire error rule=${m.sched.rule_id}:`,
        e instanceof Error ? e.message : e
      );
    }
  }
}

async function matchSchedule(supabase, sched) {
  const tz = sched.timezone || 'America/Sao_Paulo';
  const { hour, minute, dayOfWeek, now } = nowInTimezone(tz);
  const target = parseTime(sched.time_start);
  if (!target) return null;
  if (hour !== target.hour || minute !== target.minute) return null;

  if (sched.schedule_type === 'weekly') {
    if (Array.isArray(sched.days_of_week) && sched.days_of_week.length > 0) {
      if (!sched.days_of_week.includes(dayOfWeek)) return null;
    }
  }

  if (sched.schedule_type === 'grow_week') {
    if (sched.grow_week_index != null) {
      const currentWeek = await getCurrentGrowWeek(supabase, sched.device_id);
      if (currentWeek == null || currentWeek !== sched.grow_week_index) {
        return null;
      }
    }
  }

  if (sched.last_triggered_at) {
    const lastTrigger = new Date(sched.last_triggered_at);
    const diffMs = now.getTime() - lastTrigger.getTime();
    if (diffMs < 90_000) return null;
  }

  if (isFnCirculationRuleId(sched.rule_id)) {
    console.log(`[scheduler] skip_fn rule=${sched.rule_id} (tipagem/Motor, no alarma)`);
    return null;
  }

  if (!DEVICE_ID_RE.test(sched.device_id)) {
    console.warn(`[scheduler] invalid device_id ${sched.device_id}`);
    return null;
  }

  // Procedure puede estar enabled=false (Ativar la enciende). Simple: preferir enabled.
  const { data: rules, error: ruleErr } = await supabase
    .from('decision_rules')
    .select(
      'rule_id, rule_name, rule_description, rule_json, device_id, enabled, priority'
    )
    .eq('rule_id', sched.rule_id)
    .eq('device_id', sched.device_id)
    .limit(1);

  if (ruleErr) {
    console.error(
      `[scheduler] fetch decision_rules error for ${sched.rule_id}:`,
      ruleErr.message
    );
    return null;
  }
  if (!rules || rules.length === 0) {
    console.warn(
      `[scheduler] rule ${sched.rule_id} not found for ${sched.device_id}`
    );
    return null;
  }

  return { sched, rule: rules[0], now };
}

async function fireMatch(supabase, mqttClient, { sched, rule, now }) {
  const deviceId = sched.device_id;
  const ruleId = sched.rule_id;
  const ruleJson = rule.rule_json;

  if (isProcedureRuleJson(ruleJson)) {
    await fireProcedureAtivar(supabase, mqttClient, {
      sched,
      rule,
      deviceId,
      ruleId,
      now,
    });
    return;
  }

  // Simple: exigir enabled (comportamiento previo)
  if (rule.enabled === false) {
    console.warn(
      `[scheduler] simple rule ${ruleId} disabled — skip (habilite o use procedure)`
    );
    return;
  }

  const actions = extractActions(ruleJson);
  if (actions.length === 0) {
    console.warn(`[scheduler] rule ${ruleId} has no relay actions (not procedure)`);
    return;
  }

  for (const act of actions) {
    await fireScheduledCommand(supabase, mqttClient, {
      deviceId,
      ruleId,
      ruleName: rule.rule_name || ruleId,
      relayIndex: act.relay_index,
      action: act.action,
      durationS: act.duration_s,
      targetDeviceId: act.target_device_id,
    });
  }

  await markTriggered(supabase, sched.id, now);
  console.log(
    `[scheduler] simple rule=${ruleId} device=${deviceId} actions=${actions.length}`
  );
}

async function fireProcedureAtivar(supabase, mqttClient, ctx) {
  const { sched, rule, deviceId, ruleId, now } = ctx;
  const by = `${SCHEDULER_BY_PREFIX}${ruleId}`;

  const { error: updErr } = await supabase
    .from('decision_rules')
    .update({ enabled: true, updated_at: new Date().toISOString() })
    .eq('device_id', deviceId)
    .eq('rule_id', ruleId);

  if (updErr) {
    console.error(`[scheduler] enable DB failed rule=${ruleId}:`, updErr.message);
    // Seguir intentando MQTT — peor no disparar por RLS menor
  }

  const { error: histErr } = await supabase.from('rule_config_events').insert({
    device_id: deviceId,
    rule_id: ruleId,
    rule_name: rule.rule_name ?? null,
    event_type: 'enabled',
    created_by: by,
    created_at: new Date().toISOString(),
  });
  if (histErr) {
    console.warn(`[scheduler] rule_config_events skip:`, histErr.message);
  }

  const upsertRow = {
    rule_id: ruleId,
    rule_name: rule.rule_name,
    rule_description: rule.rule_description,
    rule_json: rule.rule_json,
    enabled: true,
    priority: rule.priority ?? 50,
  };

  const up = await publishRuleUpsertMqtt(mqttClient, deviceId, upsertRow);
  if (!up.ok && !up.skipped) {
    console.error(`[scheduler] procedure upsert MQTT fail rule=${ruleId}`);
    // No marcar last_triggered — permitirá reintento en ~90s+ próximo minuto
    return;
  }

  await sleep(ATIVAR_DELAY_MS);

  const cmd = await publishProcedureCmdMqtt(mqttClient, deviceId, ruleId, 'start');
  if (!cmd.ok && !cmd.skipped) {
    console.error(`[scheduler] procedure/cmd start fail rule=${ruleId}`);
    return;
  }

  await markTriggered(supabase, sched.id, now);
  console.log(
    `[scheduler] procedure start rule=${ruleId} device=${deviceId} type=${sched.schedule_type}`
  );
}

async function markTriggered(supabase, scheduleId, now) {
  const { error: updErr } = await supabase
    .from('rule_schedules')
    .update({ last_triggered_at: now.toISOString() })
    .eq('id', scheduleId);
  if (updErr) {
    console.error(
      `[scheduler] update last_triggered_at error for ${scheduleId}:`,
      updErr.message
    );
  }
}

async function fireScheduledCommand(supabase, mqttClient, opts) {
  const { deviceId, ruleId, ruleName, relayIndex, action, durationS, targetDeviceId } =
    opts;

  const insertRow = {
    device_id: deviceId,
    relay_number: relayIndex,
    action,
    status: 'pending',
    created_by: `${SCHEDULER_BY_PREFIX}${ruleId}`,
    command_type: 'rule',
    priority: 50,
    triggered_by: 'scheduler',
  };
  if (durationS > 0) {
    insertRow.duration_seconds = durationS;
  }
  if (targetDeviceId) {
    insertRow.target_device_id = targetDeviceId;
  }

  const { data, error } = await supabase
    .from('relay_commands')
    .insert(insertRow)
    .select('id')
    .single();

  if (error) {
    console.error(
      `[scheduler] INSERT relay_commands failed rule=${ruleId} relay=${relayIndex}:`,
      error.message
    );
    return;
  }

  const commandId = data.id;
  const mqttPayload = {
    v: 1,
    id: commandId,
    cmd: 'relay',
    device_id: deviceId,
    relay_index: relayIndex,
    action,
    duration_s: durationS || 0,
    source: 'api',
    command_type: 'rule',
    priority: 50,
    triggered_by: `${SCHEDULER_BY_PREFIX}${ruleId}`,
  };
  if (targetDeviceId) {
    mqttPayload.target_device_id = targetDeviceId;
    mqttPayload.slave_mac_address = targetDeviceId;
  }
  if (ruleId) mqttPayload.rule_id = ruleId;
  if (ruleName) mqttPayload.rule_name = ruleName;

  const topic = `hidrowave/${deviceId}/command`;
  const payload = JSON.stringify(mqttPayload);

  await new Promise((resolve) => {
    mqttClient.publish(topic, payload, { qos: 1 }, (err) => {
      if (err) {
        console.error(`[scheduler] MQTT publish failed cmd=${commandId}:`, err.message);
      } else {
        console.log(
          `[scheduler] MQTT command published id=${commandId} relay=${relayIndex} → ${topic}`
        );
      }
      resolve();
    });
  });
}

async function getCurrentGrowWeek(supabase, deviceId) {
  const { data, error } = await supabase
    .from('device_status')
    .select('metadata')
    .eq('device_id', deviceId)
    .single();

  if (error || !data?.metadata?.grow_start_date) return null;

  const start = new Date(data.metadata.grow_start_date);
  if (isNaN(start.getTime())) return null;

  const now = new Date();
  const diffMs = now.getTime() - start.getTime();
  if (diffMs < 0) return null;

  return Math.floor(diffMs / (7 * 24 * 60 * 60 * 1000));
}
