import 'dotenv/config';
import ws from 'ws';
import { createClient } from '@supabase/supabase-js';

const url = process.env.SUPABASE_URL;
const key = process.env.SUPABASE_SERVICE_ROLE_KEY || process.env.SUPABASE_KEY;
console.log('url', url?.slice(0, 48));
console.log('keyLen', key?.length, 'keyPrefix', key?.slice(0, 12));

const supabase = createClient(url, key, {
  auth: { persistSession: false, autoRefreshToken: false },
  realtime: { transport: ws },
});

const seq = `debug-qty-${Date.now()}`;
const { data: inc, error: e1 } = await supabase.rpc('increment_pump_quantity', {
  p_device_id: 'ESP32_HIDRO_1A575C',
  p_relay_index: 3,
  p_ml: 1.5,
  p_sequence_id: seq,
  p_role: 'ec',
});
console.log('rpc', JSON.stringify({ inc, e1 }));

const { data: all, error: e2 } = await supabase
  .from('pump_quantity')
  .select('*')
  .eq('device_id', 'ESP32_HIDRO_1A575C');
console.log('select all', JSON.stringify({ all, e2 }));

const { data: one, error: e3 } = await supabase
  .from('pump_quantity')
  .select('*')
  .eq('device_id', 'ESP32_HIDRO_1A575C')
  .eq('relay_index', 3);
console.log('select r3', JSON.stringify({ one, e3 }));
