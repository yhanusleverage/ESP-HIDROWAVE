#!/usr/bin/env node
/**
 * Bancada semi-automática: KPI toggle slave ESP-NOW (MQTT → master → slave → UI).
 *
 * Uso na VM Lightsail (/opt/hidrowave-bridge):
 *   SLAVE_MAC=14:33:5C:38:BF:60 TEST_DEVICE_ID=ESP32_HIDRO_1A575C \
 *   node scripts/bancada-slave-relay-kpi.js
 *
 * Pasos automáticos:
 *   1. Publica comando MQTT toggle slave (test-publish-slave-command.js)
 *   2. Muestra journalctl bridge (últimas líneas PATCH relay_slaves)
 *   3. Consulta relay_slaves en Supabase (check-relay-slave-row.js)
 *
 * Pasos MANUALES (obligatorios en bancada):
 *   - Cronómetro: clic UI toggle relé 0 → ACK serial master → UI actualizada (<2s objetivo)
 *   - Confirmar clic físico del relé en RelayBox
 *   - Slave serial: sin MASTER NÃO ENCONTRADO ni error 0x3069
 */
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const scriptsDir = __dirname;

const slaveMac = process.env.SLAVE_MAC || '14:33:5C:38:BF:60';
const deviceId = process.env.TEST_DEVICE_ID || 'ESP32_HIDRO_1A575C';
const relayIndex = process.env.TEST_RELAY_INDEX || '0';

function runNode(script, extraEnv = {}) {
  const env = { ...process.env, ...extraEnv };
  const r = spawnSync('node', [path.join(scriptsDir, script)], {
    env,
    stdio: 'inherit',
    shell: process.platform === 'win32',
  });
  return r.status === 0;
}

function runShell(cmd) {
  console.log('\n$ ' + cmd);
  const r = spawnSync(cmd, { shell: true, stdio: 'inherit' });
  return r.status === 0;
}

console.log('=== BANCADA SLAVE RELAY KPI ===');
console.log('Device:', deviceId);
console.log('Slave MAC:', slaveMac);
console.log('Relay:', relayIndex);
console.log('Objetivo latencia toggle→ACK→UI: < 2s (slave estable, mismo canal RF)\n');

console.log('--- [1/3] MQTT test publish slave toggle ---');
const mqttOk = runNode('test-publish-slave-command.js', {
  TEST_SLAVE_MAC: slaveMac,
  TEST_DEVICE_ID: deviceId,
  TEST_RELAY_INDEX: relayIndex,
  TEST_MODE: 'instant',
});

console.log('\n--- [2/3] Bridge journal (PATCH relay_slaves) ---');
if (process.env.SKIP_JOURNALCTL === '1') {
  console.log('SKIP_JOURNALCTL=1 — omitido');
} else {
  runShell(
    'journalctl -u hidrowave-bridge -n 40 --no-pager 2>/dev/null | grep -E "PATCH relay_slaves|Rejected|slave" || echo "(journalctl no disponible en este host)"'
  );
}

console.log('\n--- [3/3] Supabase relay_slaves row ---');
runNode('check-relay-slave-row.js', { SLAVE_MAC: slaveMac });

console.log('\n=== CHECKLIST MANUAL (bancada) ===');
console.log('[ ] Master serial: [RELAY-ACK] + [CMD ACK-DIRECT] antes de ACK-FALLBACK');
console.log('[ ] Slave serial: RELAY_ACK enviado; sin MASTER ENCONTRADO en cada PONG');
console.log('[ ] Slave serial: sin Auto-discovery cada 30s con canal locked');
console.log('[ ] GPIO: relé físico cambia de estado');
console.log('[ ] Cronómetro UI toggle → estado UI actualizado: _____ s (objetivo <2s)');
console.log('[ ] relay_commands.status = completed en Supabase');

process.exit(mqttOk ? 0 : 1);
