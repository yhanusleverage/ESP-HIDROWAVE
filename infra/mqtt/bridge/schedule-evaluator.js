/**
 * Schedule Evaluator — avalia rule_schedules a cada 60s e dispara comandos MQTT.
 *
 * Reutiliza o caminho existente:
 *   INSERT relay_commands (pending) → MQTT command → Core → command_ack → complete
 */

const DEVICE_ID_RE = /^ESP32_HIDRO_[0-9A-F]{6}$/;

/**
 * Retorna a hora/minuto/dia atuais no timezone configurado.
 * Usa Intl.DateTimeFormat (nativo Node ≥18, sem dependências).
 */
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

/**
 * Parseia time_start "HH:MM:SS" ou "HH:MM" → { hour, minute }
 */
function parseTime(timeStr) {
  if (!timeStr) return null;
  const parts = String(timeStr).split(':');
  if (parts.length < 2) return null;
  return { hour: Number(parts[0]), minute: Number(parts[1]) };
}

/**
 * Extrae acciones relay del rule_json de decision_rules.
 * Retorna array de { relay_index, action, duration_s, target_device_id }.
 */
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
      else if (type === 'relay_pulse' || type === 'pulse' || type === 'toggle') actionStr = 'on';

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
 * Evalúa todos los rule_schedules y dispara comandos cuando corresponde.
 *
 * @param {import('@supabase/supabase-js').SupabaseClient} supabase
 * @param {import('mqtt').MqttClient} mqttClient
 */
export async function evaluateSchedules(supabase, mqttClient) {
  // 1. Fetch enabled schedules
  const { data: schedules, error: schErr } = await supabase
    .from('rule_schedules')
    .select('*')
    .eq('enabled', true);

  if (schErr) {
    console.error('[scheduler] fetch rule_schedules error:', schErr.message);
    return;
  }
  if (!schedules || schedules.length === 0) return;

  // 2. Evaluate each schedule
  for (const sched of schedules) {
    try {
      await evaluateOne(supabase, mqttClient, sched);
    } catch (e) {
      console.error(`[scheduler] error evaluating schedule ${sched.id}:`, e.message);
    }
  }
}

async function evaluateOne(supabase, mqttClient, sched) {
  const tz = sched.timezone || 'America/Sao_Paulo';
  const { hour, minute, dayOfWeek, now } = nowInTimezone(tz);
  const target = parseTime(sched.time_start);
  if (!target) return;

  // Match minuto exacto
  if (hour !== target.hour || minute !== target.minute) return;

  // Si tiene time_end, verificar que estamos dentro de la ventana (no disparar, solo validar)
  // Para v1 solo disparamos al time_start exacto.

  // Verificar dias_of_week
  if (sched.schedule_type === 'weekly') {
    if (Array.isArray(sched.days_of_week) && sched.days_of_week.length > 0) {
      if (!sched.days_of_week.includes(dayOfWeek)) return;
    }
  }

  // grow_week: verificar contra device_status o metadata
  if (sched.schedule_type === 'grow_week') {
    if (sched.grow_week_index != null) {
      const currentWeek = await getCurrentGrowWeek(supabase, sched.device_id);
      if (currentWeek == null || currentWeek !== sched.grow_week_index) return;
    }
  }

  // Dedup: no repetir en el mismo minuto
  if (sched.last_triggered_at) {
    const lastTrigger = new Date(sched.last_triggered_at);
    const diffMs = now.getTime() - lastTrigger.getTime();
    if (diffMs < 90_000) return; // menos de 90s desde el último trigger
  }

  // Match — buscar la regla
  const { data: rules, error: ruleErr } = await supabase
    .from('decision_rules')
    .select('rule_id, rule_name, rule_json, device_id')
    .eq('rule_id', sched.rule_id)
    .eq('device_id', sched.device_id)
    .eq('enabled', true)
    .limit(1);

  if (ruleErr) {
    console.error(`[scheduler] fetch decision_rules error for ${sched.rule_id}:`, ruleErr.message);
    return;
  }
  if (!rules || rules.length === 0) {
    console.warn(`[scheduler] rule ${sched.rule_id} not found or disabled for ${sched.device_id}`);
    return;
  }

  const rule = rules[0];
  const actions = extractActions(rule.rule_json);
  if (actions.length === 0) {
    console.warn(`[scheduler] rule ${sched.rule_id} has no relay actions`);
    return;
  }

  const deviceId = sched.device_id;
  if (!DEVICE_ID_RE.test(deviceId)) {
    console.warn(`[scheduler] invalid device_id ${deviceId}`);
    return;
  }

  // Disparar cada acción
  for (const act of actions) {
    await fireScheduledCommand(supabase, mqttClient, {
      deviceId,
      ruleId: sched.rule_id,
      ruleName: rule.rule_name || sched.rule_id,
      relayIndex: act.relay_index,
      action: act.action,
      durationS: act.duration_s,
      targetDeviceId: act.target_device_id,
    });
  }

  // Actualizar last_triggered_at
  const { error: updErr } = await supabase
    .from('rule_schedules')
    .update({ last_triggered_at: now.toISOString() })
    .eq('id', sched.id);

  if (updErr) {
    console.error(`[scheduler] update last_triggered_at error for ${sched.id}:`, updErr.message);
  }

  console.log(
    `[scheduler] triggered rule=${sched.rule_id} device=${deviceId} actions=${actions.length} type=${sched.schedule_type}`
  );
}

/**
 * INSERT relay_commands (pending) + MQTT command — mismo camino que manual UI.
 */
async function fireScheduledCommand(supabase, mqttClient, opts) {
  const { deviceId, ruleId, ruleName, relayIndex, action, durationS, targetDeviceId } = opts;

  // INSERT relay_commands pending
  const insertRow = {
    device_id: deviceId,
    relay_number: relayIndex,
    action,
    status: 'pending',
    created_by: `scheduler#${ruleId}`,
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

  // Publish MQTT command (mismo formato que frontend)
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
    triggered_by: `scheduler#${ruleId}`,
  };
  if (targetDeviceId) {
    mqttPayload.target_device_id = targetDeviceId;
    mqttPayload.slave_mac_address = targetDeviceId;
  }
  if (ruleId) mqttPayload.rule_id = ruleId;
  if (ruleName) mqttPayload.rule_name = ruleName;

  const topic = `hidrowave/${deviceId}/command`;
  const payload = JSON.stringify(mqttPayload);

  mqttClient.publish(topic, payload, { qos: 1 }, (err) => {
    if (err) {
      console.error(`[scheduler] MQTT publish failed cmd=${commandId}:`, err.message);
    } else {
      console.log(
        `[scheduler] MQTT command published id=${commandId} relay=${relayIndex} action=${action} → ${topic}`
      );
    }
  });
}

/**
 * Obtiene la semana actual del ciclo de cultivo para un dispositivo.
 * Busca en device_status.metadata.grow_start_date y calcula la diferencia.
 */
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
