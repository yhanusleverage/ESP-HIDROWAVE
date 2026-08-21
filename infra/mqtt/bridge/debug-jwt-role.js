import 'dotenv/config';

const key = process.env.SUPABASE_SERVICE_ROLE_KEY || process.env.SUPABASE_KEY || '';
const parts = key.split('.');
if (parts.length < 2) {
  console.log('bad key');
  process.exit(1);
}
const payload = JSON.parse(Buffer.from(parts[1], 'base64url').toString('utf8'));
console.log('jwt.role=', payload.role);
console.log('jwt.ref=', payload.ref);
console.log('has_service_name=', Boolean(process.env.SUPABASE_SERVICE_ROLE_KEY));
console.log('has_SUPABASE_KEY=', Boolean(process.env.SUPABASE_KEY));
