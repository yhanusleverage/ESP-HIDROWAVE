import 'dotenv/config';
import ws from 'ws';
import { createClient } from '@supabase/supabase-js';
import { isProcedureRuleJson } from './schedule-evaluator.js';

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

const { data, error } = await sb
  .from('decision_rules')
  .select('rule_id, rule_name, enabled, rule_json')
  .eq('device_id', 'ESP32_HIDRO_1A575C');

if (error) {
  console.error(error.message);
  process.exit(1);
}

for (const r of data || []) {
  const kind = r.rule_json && r.rule_json.procedure_kind ? r.rule_json.procedure_kind : '';
  console.log(
    [
      r.rule_id,
      r.rule_name,
      `enabled=${r.enabled}`,
      `proc=${isProcedureRuleJson(r.rule_json)}`,
      `kind=${kind}`,
    ].join(' | ')
  );
}
