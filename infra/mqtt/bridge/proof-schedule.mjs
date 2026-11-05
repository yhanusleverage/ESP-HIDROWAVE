import 'dotenv/config';
import ws from 'ws';
import { createClient } from '@supabase/supabase-js';

const url = process.env.SUPABASE_URL;
const key = process.env.SUPABASE_SERVICE_ROLE_KEY;
if (!url || !key) {
  console.error('missing supabase env');
  process.exit(1);
}

const sb = createClient(url, key, {
  auth: { persistSession: false, autoRefreshToken: false },
  realtime: { transport: ws },
});

const deviceId = 'ESP32_HIDRO_1A575C';
const ruleId = 'RULE_1789183943645';

const target = new Date(Date.now() + 2 * 60 * 1000);
const parts = new Intl.DateTimeFormat('en-GB', {
  timeZone: 'America/Sao_Paulo',
  hour: '2-digit',
  minute: '2-digit',
  hourCycle: 'h23',
}).formatToParts(target);
const hh = parts.find((p) => p.type === 'hour').value;
const mm = parts.find((p) => p.type === 'minute').value;
const timeStart = `${hh}:${mm}`;

const { data, error } = await sb
  .from('rule_schedules')
  .insert({
    device_id: deviceId,
    rule_id: ruleId,
    schedule_type: 'daily',
    time_start: timeStart,
    timezone: 'America/Sao_Paulo',
    enabled: true,
    created_by: 'scheduler-proof',
  })
  .select('id, time_start, enabled, rule_id')
  .single();

if (error) {
  console.error(error.message);
  process.exit(1);
}

console.log(`proof id=${data.id} time_start=${data.time_start} rule=${data.rule_id}`);
