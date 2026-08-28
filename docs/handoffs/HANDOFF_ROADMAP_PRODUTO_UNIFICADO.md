# Handoff unificado — Roadmap produto HIDROWAVE Master

**Data:** 28/08/2026  
**Escopo:** ESP32 Master (`ESP-HIDROWAVE-main`) + bridge MQTT + HIDROWAVE web  
**Audiência:** firmware, backend, produto, operações de campo

---

## 1. Estado atual (baseline pós-sessão)

### Conectividade — matriz canónica

| Dado / acção | Canal primário | Fallback | Tabela / destino Supabase |
|--------------|----------------|----------|---------------------------|
| **Comando relé (entrega)** | MQTT `hidrowave/{id}/command` QoS1 | ~~HTTPS poll~~ **removido** (`COMMAND_POLL_HTTPS_DISABLED=1`) | `relay_commands` INSERT (UI) |
| **Comando relé (ACK)** | MQTT `.../command_ack` → bridge → RPC | HTTPS `completeRelayCommand` (ESP) | `relay_commands` → completed |
| **Config Auto EC/pH** | MQTT retained `.../ec/config`, `.../ph/config` | NVS local | `ec_config_view` / `ph_config_view` |
| **Ganho K EC/pH aprendido** | MQTT `.../ec_gain`, `.../ph_gain` → bridge → PATCH | ~~HTTPS PATCH directo ESP~~ **removido** (`GAIN_PATCH_HTTPS_DISABLED=1`) | `k_value`, `k_acid`, `k_base` nas views |
| **Telemetria sensores** | MQTT `.../telemetry` → bridge | HTTPS se MQTT offline | `hydro_measurements`, `environment_data` |
| **Heartbeat / online** | MQTT `.../heartbeat` | HTTPS `device_status` se MQTT offline | `device_status` |
| **Métricas dosagem** | MQTT `.../ec_metric`, `.../ph_metric` | — | `ec_controller_metrics`, `ph_controller_metrics` |
| **Estado relés UI** | MQTT `.../relay/state` + Realtime | Sync HTTPS relay_master (backup) | `relay_master`, `relay_slaves` |
| **Web admin local :80** | Desligado prod (`ENABLE_LOCAL_ADMIN_HTTP=0`) | Portal WiFi SoftAP | — |

### Cores ESP32

| Core | Responsabilidade |
|------|------------------|
| **0** | WiFi stack + `espNowTask` (ESP-NOW slaves) |
| **1** | `loop()` → `HydroSystemCore` → sensores, Auto EC/pH, MQTT, HTTPS pontual |

---

## 2. Melhorias propostas — priorização

### P0 — Produto premium (confiabilidade percebida)

| # | Melhoria | Por quê | Esforço | Estado |
|---|----------|---------|---------|--------|
| P0.1 | Matriz de conectividade documentada + UI “estado do canal” | Utilizador premium precisa saber *por que* relé/config não aplicou | M | **Este doc** |
| P0.2 | Comandos só MQTT (sem poll HTTPS) | Heap + simplicidade | S | ✅ `COMMAND_POLL_HTTPS_DISABLED=1` |
| P0.3 | Ganhos K EC/pH só MQTT (`ec_gain` / `ph_gain`) | Zero SSL no hot path pós-dosagem | S | ✅ nesta sessão |
| P0.4 | CI `pio run -e esp32dev` em cada PR | Firmware que não compila não shipa | S | ⬜ Pendente |
| P0.5 | Topic MQTT `.../health` (heap, mqtt_age, ec_op, slaves) + alertas | Operação de flota | M | ⬜ Pendente |
| P0.6 | OTA firmado + canal stable/beta | Campo sem visita técnica | L | ⬜ Pendente |

### P1 — Engenharia (dívida técnica)

| # | Melhoria | Por quê | Esforço |
|---|----------|---------|---------|
| P1.1 | `MQTT_HYDRO_ONLY=1` + `MQTT_HEALTH_ONLY=1` em prod | Menos SSL paralelo = menos OOM | S |
| P1.2 | Cola SSL única prioritizada (ack > k_gain > status > telemetria HTTPS) | Core 1 saturado | M |
| P1.3 | Partir god files (`SupabaseClient`, `HydroSystemCore`, `HydroControl`) | Manutenção / reviews | L |
| P1.4 | Remover código morto (`RelayBridge`, `HydroSupaManager` loop, `processSlaveRelayCommands`) | Superfície de bug | S |
| P1.5 | Build profiles `[env:prod]` vs `[env:bench]` | Flags previsíveis | S |
| P1.6 | ACK comando: considerar remover fallback HTTPS quando MQTT estável >60s | Menos SSL no ACK | S |

### P1 — Produto / UX

| # | Melhoria | Por quê |
|---|----------|---------|
| P1.7 | UI comando `pending` + “reenviar MQTT” se equipamento offline | MQTT-only entrega |
| P1.8 | UI estado máquina (`ec_operation`, `ph_operation`, conectividade) | Premium = transparência |
| P1.9 | Confirmação “config aplicada no ESP” após retained `ec/config` | Fechar loop web↔device |
| P1.10 | Política ESP-NOW lock visível na UI durante dosagem / pairing | Menos tickets “relé não respondeu” |

### P2 — Escala

| # | Melhoria |
|---|----------|
| P2.1 | Tests host-side (parsers MQTT, dedup, `ecConfigUnchanged`) |
| P2.2 | Soak 24h automatizado (heap, dup cmds, pending stale) |
| P2.3 | HMI S3: alarmes + override local sem cloud |
| P2.4 | MQTT TLS 8883 + credenciais por device rotativas |
| P2.5 | DecisionEngine: regras críticas 100% locais documentadas |

---

## 3. Roadmap sugerido (90 dias)

```
Semanas 1–2   P0.4 CI + P1.1 flags prod + P1.4 limpeza código morto
Semanas 3–4   P0.5 health MQTT + alertas Supabase
Semanas 5–6   P1.7/P1.8 UI conectividade + pending commands
Semanas 7–10  P0.6 OTA firmado
Semanas 11–12 P1.3 refactor CommandPipeline / TelemetryPipeline (incremental)
```

---

## 4. Ganhos K — migração MQTT (ec_gain / ph_gain)

### Problema anterior

Após Auto EC/pH + recirculação, o firmware aprendia `k_value` / `k_acid` / `k_base` (NVS) e fazia **PATCH HTTPS** directo a `ec_config_view` / `ph_config_view`. Isso abria SSL no Core 1 no pior momento (pós-dosagem).

### Solução

| Topic | Payload | Bridge | Supabase |
|-------|---------|--------|----------|
| `hidrowave/{id}/ec_gain` | `{ v, device_id, ts, k_value }` | PATCH | `ec_config_view.k_value` |
| `hidrowave/{id}/ph_gain` | `{ v, device_id, ts, k_acid, k_base }` | PATCH | `ph_config_view.k_acid`, `k_base` |

**Nota:** `k_*` **não** vão em `ec/config` retained (UI exclui via `DROP_KEYS` em `mqtt-controller-config.ts`) — são **saída do firmware**, não entrada da UI.

### Diferença vs `ec_metric` / `ph_metric`

| Topic | Destino | Propósito |
|-------|---------|-----------|
| `ec_metric` | `ec_controller_metrics` | Histórico por evento de dosagem |
| `ec_gain` | `ec_config_view.k_value` | Config persistente “K actual da malha” |

### ACL (Mosquitto)

```
# bridge_internal
topic read hidrowave/+/ec_gain
topic read hidrowave/+/ph_gain

# ESP (%c = client id)
topic write hidrowave/%c/ec_gain
topic write hidrowave/%c/ph_gain
```

### Validação bancada

1. Flash Master com `ENABLE_MQTT=1`, `GAIN_PATCH_HTTPS_DISABLED=1`
2. Correr ciclo Auto EC até `learnEcGainAfterSequence`
3. Serial: `[MQTT] ec_gain k=0.xxxx`
4. Bridge: `[bridge] ec_gain PATCH ec_config_view device=...`
5. UI `PhCalibrationSection`: `ec_config.k_value` actualizado (Realtime ou refresh)
6. Repetir para pH → `ph_gain`

---

## 5. Ficheiros tocados nesta migração K→MQTT

| Ficheiro | Alteração |
|----------|-----------|
| `include/Config.h` | `GAIN_PATCH_HTTPS_DISABLED=1` |
| `include/MqttClient.h` | `publishEcGain`, `publishPhGain` |
| `src/MqttClient.cpp` | topics + publish |
| `src/HydroSystemCore.cpp` | `handleEcGainLearned` / `handlePhGainLearned` → MQTT |
| `src/SupabaseClient.cpp` | PATCH gains sob `#if !GAIN_PATCH_HTTPS_DISABLED` |
| `infra/mqtt/bridge/index.js` | handlers `ec_gain`, `ph_gain` |
| `infra/mqtt/mosquitto/acl.production` | read/write topics |
| `docs/mqtt/ACL_MAPA_FUNCIONALIDADES_27AGO2026.md` | referência |

---

## 6. O que NÃO fazer agora

- Reintroduzir poll HTTPS de comandos
- GET HTTPS `ec_config_view` no loop (heap/SSL)
- Reativar web admin `:80` em produção
- Reescrever firmware noutra linguagem

---

## 7. Referências

- `docs/mqtt/HANDOFF_FASE3_COMANDOS_HIBRIDOS.md` — comandos MQTT (histórico)
- `docs/handoffs/firmware/ESP32_MASTER_RESOURCE_MAP.md` — cores / heap
- `docs/mqtt/ACL_MAPA_FUNCIONALIDADES_27AGO2026.md` — ACL broker
- `HIDROWAVE-main/src/lib/mqtt-controller-config.ts` — DROP_KEYS k_value

---

**Próximo passo recomendado:** P0.4 CI compile + deploy bridge com `ec_gain`/`ph_gain` + `align-broker-production.sh` na VM.
