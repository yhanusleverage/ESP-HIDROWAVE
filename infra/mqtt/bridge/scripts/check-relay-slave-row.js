/**
 * Diagnóstico: relay_states vs last_update en Supabase para un slave.
 * Uso: SLAVE_MAC=14:33:5C:38:BF:60 node scripts/check-relay-slave-row.js
 */
import 'dotenv/config';
import { createClient } from '@supabase/supabase-js';

const url = process.env.SUPABASE_URL;
const key = process.env.SUPABASE_SERVICE_ROLE_KEY;
const mac = (process.env.SLAVE_MAC || '14:33:5C:38:BF:60').toUpperCase();

if (!url || !key) {
  console.error('Faltan SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY');
  process.exit(1);
}

const supabase = createClient(url, key);

const { data, error } = await supabase
  .from('relay_slaves')
  .select('device_id, slave_mac_address, relay_states, relay_names, last_update, updated_at')
  .ilike('slave_mac_address', `%${mac.replace(/:/g, '%')}%`);

if (error) {
  console.error(error.message);
  process.exit(1);
}

if (!data?.length) {
  console.log(`Sin filas relay_slaves para MAC ${mac}`);
  process.exit(0);
}

for (const row of data) {
  const onCount = (row.relay_states || []).filter(Boolean).length;
  const ageSec = row.last_update
    ? Math.round((Date.now() - new Date(row.last_update).getTime()) / 1000)
    : null;
  console.log(
    JSON.stringify(
      {
        device_id: row.device_id,
        slave_mac_address: row.slave_mac_address,
        relays_on: onCount,
        relay_states: row.relay_states,
        relay_names: row.relay_names,
        last_update: row.last_update,
        age_sec: ageSec,
      },
      null,
      2
    )
  );
}
