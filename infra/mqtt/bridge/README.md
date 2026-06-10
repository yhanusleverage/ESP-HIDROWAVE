# HIDROWAVE Bridge — Telemetria + Heartbeat + Presença + Auto EC UX

| Tópico | QoS | Ação |
|--------|-----|------|
| `hidrowave/+/telemetry` | 0 | INSERT `hydro_measurements` |
| `hidrowave/+/heartbeat` | 0 | PATCH `device_status` (saúde + presença) |
| `hidrowave/+/status` | 0 | PATCH `device_status.is_online` (LWT `online:false`) |
| `hidrowave/+/ec_operation` | 0 | PATCH `relay_master.ec_operation_*` |
| `hidrowave/+/dose` | 1 | INSERT `nutrient_dosages` |

`system_health_metrics` é **VIEW read-only** sobre `device_status`. Executar `scripts/migrate_system_health_metrics.sql` só se quiseres recriar a view com `reboot_count`/`firmware_version`.

## Deploy rápido (Lightsail)

```bash
# Na VM Ubuntu
sudo useradd -r -s /usr/sbin/nologin hidrowave 2>/dev/null || true
sudo mkdir -p /opt/hidrowave-bridge
sudo chown $USER:$USER /opt/hidrowave-bridge

# Copiar arquivos (scp ou git clone)
cd /opt/hidrowave-bridge
npm install --production

cp .env.example .env
chmod 600 .env
# Editar .env: SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, MQTT_*

# ACL Mosquitto: bridge_internal read telemetry, heartbeat, status, ec_operation, dose
sudo mosquitto_passwd -b /var/lib/mosquitto/passwd bridge_internal 'SENHA_BRIDGE'

sudo cp hidrowave-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now hidrowave-bridge
sudo journalctl -u hidrowave-bridge -f
```

## Teste manual (Passo 1 — sem ESP)

Com bridge rodando:

```bash
# Opção A: script Node (precisa user com write no tópico — use hidrowave lab ou mqtt_ESP32_*)
TEST_DEVICE_ID=ESP32_HIDRO_269844 npm run test:pub
TEST_DEVICE_ID=ESP32_HIDRO_269844 npm run test:pub:ec-dose

# Opção B: mosquitto_pub no SSH
mosquitto_pub -h 127.0.0.1 -p 1883 -u hidrowave -P 'SENHA' \
  -t 'hidrowave/ESP32_HIDRO_269844/telemetry' \
  -m '{"v":1,"device_id":"ESP32_HIDRO_269844","ph":6.2,"temperature":24.5,"tds":850,"water_level_ok":true}'
```

Verificar Supabase SQL:

```sql
SELECT * FROM hydro_measurements
WHERE device_id = 'ESP32_HIDRO_269844'
ORDER BY created_at DESC LIMIT 5;
```

## Validação em camadas (MVP)

O schema mostrado no painel Supabase (*"for context only"*) **não** deve ser executado como migration — use-o só como referência. As regras **reais** vêm dos `CHECK` já aplicados na tabela live.

| Camada | Onde | O que filtra |
|--------|------|--------------|
| ESP HTTPS | `SupabaseClient::sendHydroData` | NaN, temp 0–50, pH 0–14, TDS 0–5000 |
| Bridge Node | `index.js` `validateReading` | JSON, `device_id`, tipos (sem faixa no MVP) |
| **Supabase DB** | `hydro_measurements_*_check` | **CHECK na tabela** — rejeita INSERT inválido |

Erro típico com sensores desconectados:

```text
Supabase insert failed: violates check constraint "hydro_measurements_temperature_check"
```

Isso significa: MQTT + bridge OK; a **BD** barrou (comportamento esperado sem hardware).

### Inspecionar constraints reais (SQL Editor)

```sql
SELECT conname, pg_get_constraintdef(oid) AS definition
FROM pg_constraint
WHERE conrelid = 'public.hydro_measurements'::regclass
  AND contype = 'c';
```

### Teste end-to-end sem ESP (valores dentro do CHECK)

```bash
mosquitto_pub -h 127.0.0.1 -u hidrowave -P 'SENHA' \
  -t 'hidrowave/ESP32_HIDRO_269844/telemetry' \
  -m '{"device_id":"ESP32_HIDRO_269844","ph":6.2,"temperature":24.5,"tds":850,"water_level_ok":true}'
```

Com sensores reais no ESP, telemetria MQTT deve passar nas três camadas sem alterar a BD.

### Teste Auto EC UX (ec_operation + dose)

```bash
mosquitto_pub -h 127.0.0.1 -u hidrowave -P 'SENHA' \
  -t 'hidrowave/ESP32_HIDRO_269844/ec_operation' \
  -m '{"v":1,"device_id":"ESP32_HIDRO_269844","ec_operation_state":"recirculating","ec_operation_remaining_sec":60,"ec_next_check_in_sec":0}'

mosquitto_pub -h 127.0.0.1 -u hidrowave -P 'SENHA' -q 1 \
  -t 'hidrowave/ESP32_HIDRO_269844/dose' \
  -m '{"v":1,"device_id":"ESP32_HIDRO_269844","sequence_id":"test-mqtt","nutrient_name":"22CCC","relay_number":3,"dosage_ml":10.5,"dosage_time_seconds":5,"source":"auto_ec"}'
```

Verificar Supabase:

```sql
SELECT ec_operation_state, ec_operation_remaining_sec FROM relay_master WHERE device_id = 'ESP32_HIDRO_269844';
SELECT * FROM nutrient_dosages WHERE device_id = 'ESP32_HIDRO_269844' ORDER BY created_at DESC LIMIT 5;
```

UI `/automacao` deve mostrar badge recirc + última dosagem via Realtime.

## Variáveis `.env`

| Variável | Descrição |
|----------|-----------|
| `MQTT_HOST` | `127.0.0.1` se bridge na mesma VM |
| `MQTT_USER` / `MQTT_PASS` | User `bridge_internal` |
| `SUPABASE_SERVICE_ROLE_KEY` | Só no servidor |
| `TELEMETRY_THROTTLE_MS` | Default `30000` (MVP paralelo HTTPS) |
| `EC_OPERATION_THROTTLE_MS` | Default `2000` — evita PATCH spam em `relay_master` |
