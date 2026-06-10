# MVP device_status — heartbeat MQTT (fechar agora)

**Objetivo:** ESP publica saúde + presença via MQTT → bridge PATCH `device_status` → view `system_health_metrics` para UI.  
**Modo:** bivalente (`mqtt_health_only=0`) — HTTPS `updateDeviceStatus` continua em paralelo até soak validado.

---

## Arquitetura (fechada)

```
ESP32
  heartbeat 60s  → hidrowave/{device_id}/heartbeat
  LWT + online   → hidrowave/{device_id}/status (retain QoS1)

Bridge (Lightsail)
  heartbeat → PATCH device_status (wifi_rssi, free_heap, uptime_seconds, reboot_count,
                                     firmware_version, ip_address, last_seen, is_online)
  status    → PATCH device_status.is_online (false no disconnect)

device_status (tabela) ← fonte única
system_health_metrics (VIEW) ← dashboard SELECT (read-only)
```

**Não criar tabela** `system_health_metrics` — já é VIEW. Script: `scripts/migrate_system_health_metrics.sql` (DROP VIEW + CREATE).

---

## Checklist — ordem de execução

### 1. Supabase SQL

- [ ] Executar `scripts/migrate_system_health_metrics.sql` (com `DROP VIEW CASCADE`)
- [ ] Verificar:

```sql
SELECT table_type FROM information_schema.tables
WHERE table_name = 'system_health_metrics';
-- → VIEW

SELECT device_id, reboot_count, wifi_rssi, health_score, is_online, last_seen
FROM system_health_metrics
WHERE device_id = 'ESP32_HIDRO_269844';
```

### 2. Bridge Lightsail

- [ ] Copiar `infra/mqtt/bridge/index.js` para `/opt/hidrowave-bridge/`
- [ ] `.env`: `HEARTBEAT_THROTTLE_MS=55000`, `HEARTBEAT_STALE_MS=120000`
- [ ] `sudo systemctl restart hidrowave-bridge`
- [ ] Log esperado:

```
[bridge] Subscribed hidrowave/+/telemetry, hidrowave/+/heartbeat, hidrowave/+/status
[bridge] PATCH device_status ESP32_HIDRO_269844 heap=... rssi=... reboot=...
```

### 3. Firmware ESP

- [ ] `secrets.ini`: `mqtt_enabled=1`, `mqtt_health_only=0`, `mqtt_hydro_only=1`
- [ ] PlatformIO: build + upload
- [ ] Serial esperado:

```
[MQTT] Connected (LWT on status topic)
[MQTT] heartbeat heap=... rssi=... uptime=... reboot=...
📤 Status do dispositivo atualizado no Supabase   ← bivalente HTTPS
```

### 4. Validação cruzada (15 min)

| Onde | O quê |
|------|--------|
| Bridge log | `PATCH device_status` ~ cada 60s |
| Bridge log | `INSERT hydro_measurements` ~ cada 30s |
| SQL `device_status` | `last_seen` avança; `wifi_rssi`, `free_heap` coerentes |
| SQL view | `health_score`, `memory_status` calculados |
| Teste LWT | Desligar ESP → `is_online=false` em ~segundos |

```sql
SELECT device_id, is_online, last_seen, wifi_rssi, free_heap, reboot_count, updated_at
FROM device_status
WHERE device_id = 'ESP32_HIDRO_269844';
```

### 5. Soak 24h (gate para próxima fase)

- [ ] Heap estável (baseline −5KB)
- [ ] Sem reboot espontâneo
- [ ] `last_seen` contínuo via MQTT (+ HTTPS bivalente)
- [ ] Bridge sem erros em `journalctl -u hidrowave-bridge`

Ver também: [MVP_SOAK_CHECKLIST.md](./MVP_SOAK_CHECKLIST.md) (telemetria hydro).

---

## Teste manual heartbeat (SSH Lightsail)

```bash
mosquitto_pub -h 127.0.0.1 -u hidrowave -P 'SENHA' \
  -t 'hidrowave/ESP32_HIDRO_269844/heartbeat' \
  -m '{"v":1,"device_id":"ESP32_HIDRO_269844","wifi_rssi":-65,"free_heap":180000,"uptime_seconds":120,"reboot_count":1,"firmware_version":"2.1.0","ip_address":"192.168.1.50"}'
```

---

## Rollback

| Ação | Efeito |
|------|--------|
| `mqtt_enabled=0` + reflash | ESP só HTTPS para saúde |
| Bridge antigo (só telemetry) | Sem PATCH device_status via MQTT |
| `mqtt_health_only=1` (futuro) | Omitir HTTPS saúde; só MQTT |

---

## Depois deste MVP (não fazer agora)

**Fase 3 — comandos híbridos:** ver [03_PLANO_IMPLEMENTACAO_FASES.md](./03_PLANO_IMPLEMENTACAO_FASES.md) § Fase 3.
