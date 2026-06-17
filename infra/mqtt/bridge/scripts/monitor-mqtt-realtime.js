/**
 * Monitor MQTT em tempo real — espelha o que o bridge AWS recebe do ESP32.
 *
 * Uso local (túnel SSH para broker na Lightsail):
 *   ssh -L 1883:127.0.0.1:1883 ubuntu@99.79.36.220 -i sua-chave.pem
 *   cd infra/mqtt/bridge && npm run monitor
 *
 * Uso na própria VM AWS:
 *   cd /opt/hidrowave-bridge && node scripts/monitor-mqtt-realtime.js
 *
 * Env:
 *   MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS  (igual ao bridge)
 *   MONITOR_DEVICE_ID=ESP32_HIDRO_269844         (opcional — filtra device)
 *   MONITOR_TOPICS=ph,telemetry                   (opcional — só suffixes)
 */
import 'dotenv/config';
import mqtt from 'mqtt';

const host = process.env.MQTT_HOST || '127.0.0.1';
const port = process.env.MQTT_PORT || '1883';
const deviceFilter = process.env.MONITOR_DEVICE_ID || process.env.TEST_DEVICE_ID || '';
const topicFilterRaw = process.env.MONITOR_TOPICS || '';
const topicSuffixFilter = topicFilterRaw
  ? new Set(topicFilterRaw.split(',').map((s) => s.trim()).filter(Boolean))
  : null;

const TOPIC_WILDCARD = deviceFilter
  ? `hidrowave/${deviceFilter}/#`
  : 'hidrowave/#';

const COLORS = {
  reset: '\x1b[0m',
  dim: '\x1b[2m',
  bold: '\x1b[1m',
  telemetry: '\x1b[36m',
  heartbeat: '\x1b[90m',
  status: '\x1b[33m',
  ec_operation: '\x1b[34m',
  dose: '\x1b[32m',
  ph_operation: '\x1b[35m',
  ph_dose: '\x1b[95m',
  error: '\x1b[31m',
};

const stats = {
  startedAt: Date.now(),
  bySuffix: {},
  total: 0,
  parseErrors: 0,
};

function ts() {
  return new Date().toLocaleTimeString('pt-BR', { hour12: false });
}

function colorForSuffix(suffix) {
  return COLORS[suffix] || COLORS.reset;
}

function incStat(suffix) {
  stats.total += 1;
  stats.bySuffix[suffix] = (stats.bySuffix[suffix] || 0) + 1;
}

function fmtPh(v) {
  const n = Number(v);
  if (!Number.isFinite(n)) return String(v);
  if (Math.abs(n) < 0.01 || Math.abs(n) >= 1000) return n.toExponential(3);
  return n.toFixed(2);
}

function summarize(suffix, data) {
  switch (suffix) {
    case 'telemetry':
      return `ph=${fmtPh(data.ph)} ec=${data.ec ?? '--'} temp=${data.temperature ?? '--'} tds=${data.tds ?? '--'}`;
    case 'heartbeat':
      return `online=${data.online ?? true} uptime=${data.uptime_sec ?? data.uptime ?? '--'}s`;
    case 'status':
      return `online=${data.online}`;
    case 'ec_operation':
      return `state=${data.ec_operation_state} rem=${data.ec_operation_remaining_sec}s next=${data.ec_next_check_in_sec ?? 0}s`;
    case 'dose':
      return `${data.direction ?? '?'} ${data.dosage_ml}ml relay=${data.relay_number} seq=${data.sequence_id ?? '--'}`;
    case 'ph_operation':
      return `state=${data.ph_operation_state} rem=${data.ph_operation_remaining_sec}s next=${data.ph_next_check_in_sec ?? 0}s`;
    case 'ph_dose':
      return `${data.direction ?? '?'} ${data.dosage_ml}ml ph_before=${fmtPh(data.ph_before)} SP=${fmtPh(data.ph_setpoint)} seq=${data.sequence_id ?? '--'}`;
    default:
      return JSON.stringify(data).slice(0, 120);
  }
}

function printBanner() {
  console.log('');
  console.log(`${COLORS.bold}HIDROWAVE MQTT Monitor${COLORS.reset}  ${COLORS.dim}${ts()}${COLORS.reset}`);
  console.log(`${COLORS.dim}broker${COLORS.reset} mqtt://${host}:${port}`);
  console.log(`${COLORS.dim}subscribe${COLORS.reset} ${TOPIC_WILDCARD}`);
  if (deviceFilter) console.log(`${COLORS.dim}filter device${COLORS.reset} ${deviceFilter}`);
  if (topicSuffixFilter) {
    console.log(`${COLORS.dim}filter topics${COLORS.reset} ${[...topicSuffixFilter].join(', ')}`);
  }
  console.log(`${COLORS.dim}Ctrl+C${COLORS.reset} sair · stats a cada 60s`);
  console.log('─'.repeat(72));
}

function printStats() {
  const elapsed = Math.round((Date.now() - stats.startedAt) / 1000);
  const parts = Object.entries(stats.bySuffix)
    .sort((a, b) => b[1] - a[1])
    .map(([k, v]) => `${k}:${v}`)
    .join(' ');
  console.log(
    `${COLORS.dim}[stats ${ts()} +${elapsed}s] total=${stats.total} parse_err=${stats.parseErrors}${parts ? ` | ${parts}` : ''}${COLORS.reset}`
  );
}

function parseTopic(topic) {
  const parts = topic.split('/');
  if (parts.length < 3 || parts[0] !== 'hidrowave') return null;
  return { deviceId: parts[1], suffix: parts[2] };
}

const client = mqtt.connect(`mqtt://${host}:${port}`, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASS,
  reconnectPeriod: 3000,
  connectTimeout: 15000,
});

printBanner();

client.on('connect', () => {
  client.subscribe(TOPIC_WILDCARD, { qos: 0 }, (err) => {
    if (err) {
      console.error(`${COLORS.error}Subscribe failed:${COLORS.reset}`, err.message);
      process.exit(1);
    }
    console.log(`${COLORS.bold}Conectado — aguardando mensagens…${COLORS.reset}\n`);
  });
});

client.on('reconnect', () => {
  console.log(`${COLORS.dim}[${ts()}] reconectando MQTT…${COLORS.reset}`);
});

client.on('error', (err) => {
  console.error(`${COLORS.error}MQTT error:${COLORS.reset}`, err.message);
});

client.on('message', (topic, message) => {
  const parsed = parseTopic(topic);
  if (!parsed) return;

  const { deviceId, suffix } = parsed;
  if (topicSuffixFilter && !topicSuffixFilter.has(suffix)) return;

  incStat(suffix);
  const color = colorForSuffix(suffix);
  const raw = message.toString();

  let data;
  try {
    data = JSON.parse(raw);
  } catch {
    stats.parseErrors += 1;
    console.log(
      `${color}[${ts()}] ${suffix.padEnd(14)}${COLORS.reset} ${COLORS.dim}${deviceId}${COLORS.reset} ${COLORS.error}JSON inválido${COLORS.reset} ${raw.slice(0, 80)}`
    );
    return;
  }

  const summary = summarize(suffix, data);
  console.log(
    `${color}[${ts()}] ${suffix.padEnd(14)}${COLORS.reset} ${COLORS.dim}${deviceId}${COLORS.reset} ${summary}`
  );
});

const statsInterval = setInterval(printStats, 60_000);

process.on('SIGINT', () => {
  clearInterval(statsInterval);
  printStats();
  console.log(`\n${COLORS.dim}Encerrado.${COLORS.reset}`);
  client.end(true, () => process.exit(0));
});
