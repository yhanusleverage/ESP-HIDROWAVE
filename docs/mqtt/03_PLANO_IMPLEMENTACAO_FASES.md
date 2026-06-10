# 03 — Checklist operacional por fase

**Plano mestre (fases, gaps, KPIs):** [00_PLANO_MESTRE.md](./00_PLANO_MESTRE.md)  
**Confiabilidade R1–R9:** [10_REQUISITOS_CONFIABILIDADE.md](./10_REQUISITOS_CONFIABILIDADE.md)  
**Intervalos ms:** [09_INTERVALOS_REST_VS_MQTT.md](./09_INTERVALOS_REST_VS_MQTT.md)

---

## FASE 0b — Hardening broker (gate)

- [ ] IP estático Lightsail
- [ ] Firewall IPv4 TCP 1883
- [ ] User MQTT `mqtt_ESP32_HIDRO_XXXXXX` + ACL
- [ ] Rotacionar senha exposta
- [ ] `mosquitto_pub` externo OK

---

## FASE 1 — Bridge telemetria + device_status

- [x] `/opt/hidrowave-bridge/` + `.env` chmod 600
- [x] User `bridge_internal` no ACL
- [x] telemetry → INSERT `hydro_measurements`
- [x] heartbeat → PATCH `device_status` (código em `index.js`)
- [x] LWT status → `is_online=false`
- [ ] **Deploy `index.js` actualizado na VM + restart**
- [ ] Teste pub manual heartbeat → `device_status` actualiza

**Gate:** 0b completo. Checklist: [MVP_DEVICE_STATUS_HEARTBEAT.md](./MVP_DEVICE_STATUS_HEARTBEAT.md)

---

## FASE 2 — ESP publish (soak device_status + hydro)

- [x] `MqttClient.*`, heartbeat 60s, telemetry 30s, LWT
- [x] `mqtt_hydro_only=1` (hydro só MQTT)
- [x] Bivalente saúde: `mqtt_health_only=0` (HTTPS + MQTT)
- [ ] **Flash ESP com firmware heartbeat**
- [ ] SQL view `system_health_metrics` (DROP + CREATE)
- [ ] KPI 24h: heap, `last_seen`, bridge logs

**Gate:** [MVP_DEVICE_STATUS_HEARTBEAT.md](./MVP_DEVICE_STATUS_HEARTBEAT.md) completo.

---

## FASE 2b — Viável (−600 REST/h)

- [ ] `COMMAND_POLL_INTERVAL_MS` = 30000
- [ ] `SUPABASE_CHECK_INTERVAL` = 30000
- [ ] `RELAY_STATES_SYNC_INTERVAL` = 30000
- [ ] `STATUS_SEND_INTERVAL` = 120000
- [ ] HTTPS sensores OFF; telemetry MQTT 30s
- [ ] KPI: REST/h &lt; 600

---

## FASE 3 — Comandos híbridos + dedup (PRÓXIMO PASSO após soak device_status)

**Decisão (mai/2026):** `relay_commands` **permanece** — historial pending/sent/completed + UI. MQTT = push; HTTPS poll = backup.

### Arquitetura acordada

```
Dashboard → INSERT relay_commands (status: pending)   ← auditoría siempre
         → API publica hidrowave/{id}/command         ← entrega rápida (QoS1)

ESP (MQTT online)
  → subscribe command → executa → markCommandSent/Completed (HTTPS leve)

ESP (MQTT offline)
  → poll relay_commands HTTPS cada 10s (não 5s)       ← mesma arquitetura actual
```

| Modo | Comandos | Poll backup |
|------|----------|-------------|
| MQTT online | Push instantâneo | 60s (ou off) |
| MQTT offline | Só poll HTTPS | **10s** |

**Nota:** bridge **não precisa** direção inversa — ESP subscreve directo ao Mosquitto; API/backend publica após INSERT. Uso esperado: manual, calibración, bombas, peristálticas, slaves — baixo volume.

### Checklist implementação

- [ ] `MqttClient`: subscribe `hidrowave/{id}/command` QoS 1
- [ ] Payload inclui `id` (= `relay_commands.id`) — dedup NVS ~32 ids (R2)
- [ ] Rejeitar sem `id` (R9)
- [ ] API frontend: após INSERT `relay_commands` → `mosquitto_pub` ou route server-side
- [ ] Ack HTTPS `markCommandCompleted` / `markCommandFailed` (R3) — inalterado
- [ ] `COMMAND_POLL_INTERVAL_MS`: 60000 se MQTT OK, **10000** se MQTT down
- [ ] KPI: 100 cmds manuais, 0 dup, &lt;2s latência com MQTT; fallback 10s OK offline

---

## FASE 3b — Recovery gaps

- [ ] `esp_reset_reason()` → `device_status` (R5)
- [ ] Política pós-reboot relés — safe mode default (R6)
- [ ] `loadMasterRelayStatesFromNVS` conforme política
- [ ] Reboot remoto ESP (opcional)

---

## FASE 4 — Drift + ideal

- [ ] `relay/state` on-change MQTT
- [ ] Sync HTTPS 60s reconciliação (R7)
- [ ] STATUS 300s; relay_master on-change
- [ ] KPI: REST/h &lt; 350

---

## FASE 5 — Operação

- [ ] Runbook novo device
- [ ] Monitor Mosquitto + bridge
- [ ] MQTTS se necessário

---

## Rollback

| Ação | Efeito |
|------|--------|
| `ENABLE_MQTT=0` | ESP só HTTPS |
| `systemctl stop hidrowave-bridge` | UI via HTTPS ESP |
| Mosquitto off | Poll **10s** comandos se MQTT off; 60s backup se MQTT on (fase 3) |

---

## Cronograma

| Sem | Fase |
|-----|------|
| 1 | 0b + 1 |
| 2 | 2 |
| 3 | 2b |
| 4 | 3 + 3b |
| 5 | 4 + 5 |

**Agora:** fechar Fase 1+2 device_status → [MVP_DEVICE_STATUS_HEARTBEAT.md](./MVP_DEVICE_STATUS_HEARTBEAT.md)  
**Depois soak 24h:** Fase 3 comandos híbridos (secção acima).
