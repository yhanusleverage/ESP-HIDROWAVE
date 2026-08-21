/**
 * Bancada / smoke: pump_quantity increment idempotente.
 * Uso: node verify-pump-quantity.js [device_id] [relay]
 */
import 'dotenv/config';
import ws from 'ws';
import { createClient } from '@supabase/supabase-js';

const url = process.env.SUPABASE_URL;
const key = process.env.SUPABASE_SERVICE_ROLE_KEY || process.env.SUPABASE_KEY;
const deviceId = process.argv[2] || process.env.VERIFY_DEVICE_ID || 'ESP32_HIDRO_1A575C';
const relay = Number(process.argv[3] ?? 0);
const seq = `bench-qty-${Date.now()}`;
const seq2 = `${seq}-b`;

if (!url || !key) {
  console.error('Missing SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY');
  process.exit(1);
}

const supabase = createClient(url, key, {
  auth: { persistSession: false, autoRefreshToken: false },
  realtime: { transport: ws },
});

async function main() {
  const { data: inc1, error: e1 } = await supabase.rpc('increment_pump_quantity', {
    p_device_id: deviceId,
    p_relay_index: relay,
    p_ml: 2.5,
    p_sequence_id: seq,
    p_role: 'ec',
  });
  if (e1) throw e1;
  const after1 = Number(inc1?.total_ml);
  console.log(`[1] after first increment total_ml=${after1} seq=${seq}`);
  if (!(after1 >= 2.5 - 0.001)) {
    throw new Error('FAIL: total did not include 2.5');
  }

  const { data: inc2, error: e2 } = await supabase.rpc('increment_pump_quantity', {
    p_device_id: deviceId,
    p_relay_index: relay,
    p_ml: 2.5,
    p_sequence_id: seq,
    p_role: 'ec',
  });
  if (e2) throw e2;
  const after2 = Number(inc2?.total_ml);
  console.log(`[2] after replay total_ml=${after2}`);
  if (Math.abs(after2 - after1) > 0.001) {
    throw new Error('FAIL: replay duplicated ml');
  }

  const { data: inc3, error: e3 } = await supabase.rpc('increment_pump_quantity', {
    p_device_id: deviceId,
    p_relay_index: relay,
    p_ml: 1.0,
    p_sequence_id: seq2,
    p_role: 'ec',
  });
  if (e3) throw e3;
  const after3 = Number(inc3?.total_ml);
  console.log(`[3] after new seq +1ml total_ml=${after3}`);
  if (Math.abs(after3 - (after1 + 1.0)) > 0.001) {
    throw new Error('FAIL: second sequence did not add 1ml');
  }

  console.log('OK pump_quantity bench (increment + idempotency)');
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
