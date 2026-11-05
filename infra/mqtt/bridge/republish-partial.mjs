import 'dotenv/config';
import { createClient } from '@supabase/supabase-js';
import { publishRuleUpsertMqtt } from './schedule-mqtt-publish.js';

const deviceId = 'ESP32_HIDRO_1A575C';
const ruleId = 'RULE_1789183943645';

const supabase = createClient(process.env.SUPABASE_URL, process.env.SUPABASE_SERVICE_ROLE_KEY);
const { data, error } = await supabase
  .from('decision_rules')
  .select('rule_id, rule_name, rule_description, rule_json, enabled, priority')
  .eq('device_id', deviceId)
  .eq('rule_id', ruleId)
  .maybeSingle();

if (error || !data) {
  console.error(error?.message || 'ausente');
  process.exit(1);
}

data.enabled = false;
const published = await publishRuleUpsertMqtt(null, deviceId, data);
const bytes = JSON.stringify(data.rule_json || {}).length;
console.log(`republish ${ruleId} enabled=false ok=${published.ok === true} json=${bytes}`);
process.exit(published.ok ? 0 : 1);
