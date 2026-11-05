import 'dotenv/config';
import ws from 'ws';
import { createClient } from '@supabase/supabase-js';

const sb = createClient(process.env.SUPABASE_URL, process.env.SUPABASE_SERVICE_ROLE_KEY, {
  auth: { persistSession: false, autoRefreshToken: false },
  realtime: { transport: ws },
});

const id = 'fdc121a8-220d-4ff1-be24-bccd734af32b';
const { data, error } = await sb
  .from('rule_schedules')
  .update({ enabled: false })
  .eq('id', id)
  .select('id, enabled, time_start, rule_id')
  .single();

if (error) {
  console.error(error.message);
  process.exit(1);
}
console.log(`disabled id=${data.id} enabled=${data.enabled} time=${data.time_start} rule=${data.rule_id}`);
