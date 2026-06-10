# 00 — Plano mestre MQTT + Supabase (HIDROWAVE)

**Versão:** 2.0 — reescrito com confiabilidade, eficiência, gaps e estado real do código  
**Data referência:** maio/2026  
**Prioridade do produto:** integridade de comandos, sync de estado e recovery — **não** latência mínima.

---

## 1. Decisão arquitetural (fechada)

### Arquitetura adotada: **Híbrida MQTT + Supabase**

```
┌──────────────┐   MQTT :1883      ┌─────────────┐   service_role   ┌──────────────┐
│ ESP32 Master │◄─────────────────►│  Mosquitto  │◄────────────────►│ Bridge Node  │
│              │   heartbeat,      │  Lightsail  │                  │ (Lightsail)  │
│              │   telemetry,      └─────────────┘                  └──────┬───────┘
│              │   command, state                                         │
│              │   HTTPS (SSL) ───────────────────────────────────────────►│
└──────────────┘   ack, regras, backup poll, registro                      ▼
                                                                    ┌──────────────┐
┌──────────────┐   HTTPS + Realtime (opcional)                      │  Supabase    │
│ HIDROWAVE UI │◄───────────────────────────────────────────────────│  PostgreSQL  │
└──────────────┘   NUNCA MQTT direto no browser                      └──────────────┘
```

### O que **não** adotamos

| Opção | Motivo |
|-------|--------|
| MQTT puro (sem DB como verdade) | Perde auditoria, histórico e multi-usuário |
| WebSocket **no ESP32** | +RAM, +reconnect; **não** melhora integridade vs MQTT+DB |
| MQTT no browser | Credencial exposta; duplica Supabase |
| AWS IoT Core (agora) | Complexidade e custo desproporcionais |
| Broker SaaS (agora) | Migração prematura; Mosquitto Lightsail suficiente |

### Princípios (ordem de prioridade)

1. **Supabase = fonte da verdade** (UI, fila, histórico, ack persistido).
2. **MQTT = transporte leve** ESP ↔ broker (não substitui o banco).
3. **Ack explícito no Postgres** (`relay_commands.status`, `markCommandCompleted`).
4. **Idempotência por `id`** no ESP (MQTT QoS 1 + poll HTTPS backup).
5. **NVS local** para cache de relés e contadores; política pós-reboot explícita.
6. **Drift** corrigido por sync periódico HTTPS (fase 4), não só MQTT.
7. **Rollback:** `ENABLE_MQTT=0`, bridge off, ESP continua HTTPS.

---

## 2. Legenda de status

| Símbolo | Significado |
|---------|-------------|
| ✅ | Implementado e em uso |
| 🟡 | Parcial / infra ou código incompleto |
| ⬜ | Gap — não implementado |
| 📄 | Apenas documentado / template |

---

## 3. Matriz completa: requisito × estado

### 3.1 Infraestrutura e broker

| Item | Status | Detalhe |
|------|--------|---------|
| Lightsail Ubuntu + Mosquitto | ✅ | `active (running)` confirmado |
| `allow_anonymous false` | ✅ | |
| `password_file` / `acl` em `/var/lib/mosquitto/` | ✅ | Fix AppArmor |
| Listener 1883 plain | ✅ | MVP |
| User MQTT **por device** (produção) | 🟡 | Lab usa user compartilhado |
| IP estático Lightsail | ⬜ | Recomendado antes de gravar no firmware |
| Firewall IPv4 TCP 1883 | ⬜ | Validar externamente |
| Rotação senha vazada | ⬜ | |
| MQTTS :8883 | ⬜ | Fase futura |
| Templates `infra/mqtt/mosquitto/` | 📄 | `.example` no repo |

### 3.2 Bridge MQTT → Supabase

| Item | Status | Detalhe |
|------|--------|---------|
| Serviço Node `hidrowave-bridge` | ⬜ | Só `.env.example` |
| Heartbeat → `device_status.last_seen` | ⬜ | |
| LWT → `is_online=false` | ⬜ | |
| Telemetry → `hydro_measurements` | ⬜ | Throttle configurável |
| Publish `command` a partir de `relay_commands` | ⬜ | Fase 3 |
| Validação `device_id` + regex | ⬜ | |
| `.env` chmod 600, user dedicado | ⬜ | |
| systemd unit | 📄 | Spec em doc 06 |

### 3.3 Firmware ESP32 — cloud

| Item | Status | Detalhe |
|------|--------|---------|
| Supabase HTTPS (geral) | ✅ | `SupabaseClient.cpp` |
| Fila `relay_commands` poll 5 s | ✅ | `COMMAND_POLL_INTERVAL_MS` |
| `markCommandCompleted` / `markCommandFailed` | ✅ | Ack HTTPS |
| Sensores → Supabase 30 s | ✅ | |
| `device_status` 60 s | ✅ | |
| Sync relés 10 s | ✅ | |
| `decision_rules` poll 30 s | ✅ | Mantém HTTPS |
| Admin HTTP :80 | ❌ desligado | `ENABLE_LOCAL_ADMIN_HTTP=0` |
| Portal WiFi AP | ✅ | `WiFiConfigServer.cpp` |
| `PubSubClient` / `MqttClient.*` | ⬜ | |
| `ENABLE_MQTT` em `Config.h` | ⬜ | |
| `secrets.ini` mqtt_* | 🟡 | `secrets.ini.example` só |
| MQTT heartbeat / telemetry | ⬜ | |
| MQTT LWT + QoS | 📄 | Doc 04 |
| MQTT subscribe `command` | ⬜ | Fase 3 |
| Dedup por `relay_commands.id` | ⬜ | **Gap crítico fase 3** |
| Intervalos fase 2b (poll 30 s) | ⬜ | Doc 09 |

### 3.4 Confiabilidade e recovery

| Item | Status | Detalhe |
|------|--------|---------|
| NVS cache relés master (`save/load`) | 🟡 | **Save** no sync; **load não aplica** no boot |
| Política pós-reboot relés (safe vs restore) | ⬜ | Comentário no código: “não aplica automaticamente” |
| `reboot_count` NVS + envio `device_status` | ✅ | `main.cpp`, `getRebootCount()` |
| `esp_reset_reason()` → Supabase | ⬜ | **Gap** |
| Reboot remoto: front + RPC | 🟡 | API existe; ESP **não** verifica contador remoto |
| NetworkWatchdog + reboot recovery | ✅ | `NetworkWatchdog.h` |
| Mutex Supabase / heap guard SSL | ✅ | `MIN_HEAP_FOR_HTTPS` 30 KB |
| Drift + sync reconciliação 60 s | 📄 | Fase 4 |

### 3.5 Frontend

| Item | Status | Detalhe |
|------|--------|---------|
| UI só Supabase | ✅ | |
| Online via `last_seen` &lt; 5 min | ✅ | `dispositivos/page.tsx` |
| Supabase Realtime (opcional) | ⬜ | Melhoria pós-bridge |
| MQTT / WebSocket no browser | ❌ | Decisão: não |

---

## 4. Requisitos de confiabilidade (critérios de aceite)

Detalhamento em [10_REQUISITOS_CONFIABILIDADE.md](./10_REQUISITOS_CONFIABILIDADE.md).

| # | Requisito | Mecanismo | Fase |
|---|-----------|-----------|------|
| R1 | Comando não se perde | Fila `relay_commands` + MQTT QoS1 + poll backup 30 s | 2b–3 |
| R2 | Comando não executa 2× | Dedup por `id` + “já está no estado pedido” | 3 |
| R3 | Ack auditável | `markCommandCompleted` HTTPS → Postgres | ✅ hoje |
| R4 | Offline detectável | LWT + heartbeat + bridge | 1–2 |
| R5 | Estado UI ≈ hardware | on-change MQTT + sync drift 60 s | 4 |
| R6 | Sobrevive reboot | NVS + política explícita relés | **Gap → 3b** |
| R7 | Causa do reboot | `esp_reset_reason()` → `device_status` | **Gap → 3b** |
| R8 | Broker down | Poll HTTPS continua | 2b |
| R9 | Payload inválido | Rejeitar sem `id` / JSON inválido | 3 |

### QoS (decisão fechada)

| Tópico | QoS | Retain |
|--------|-----|--------|
| `heartbeat` | 0 | false |
| `telemetry` | 0 | false |
| `status` (LWT) | 1 | true |
| `relay/state` | 1 | false |
| `command` | 1 | false |

**QoS 2:** não usar no ESP (PubSubClient, overhead). **Dedup por `id` obrigatório** mesmo com QoS 1 — cobre MQTT + poll HTTPS.

---

## 5. Eficiência REST — metas numéricas

Referência completa: [09_INTERVALOS_REST_VS_MQTT.md](./09_INTERVALOS_REST_VS_MQTT.md).

| Fase | REST/h | MQTT msgs/h | Redução | Viável RAM? |
|------|--------|-------------|---------|-------------|
| Hoje | ~1.560 | 0 | — | Baseline |
| 2 (paralelo curto) | ~1.560 | ~180 | 0% | ⚠️ soak 1–2 sem |
| **2b** | ~540 | ~200 | **~65%** | ✅ meta |
| **4** | ~310 | ~220 | **~80%** | ✅ ideal |

**Regra:** MQTT só compensa se **poll 5 s → 30 s** (−600 REST/h) **ou** sensores saem do HTTPS.

### Intervalos fase 2b (quando aplicar)

| Constante | Hoje | Fase 2b |
|-----------|------|---------|
| `COMMAND_POLL_INTERVAL_MS` | 5000 | **30000** |
| `SUPABASE_CHECK_INTERVAL` | 5000 | **30000** (igual ao anterior) |
| `RELAY_STATES_SYNC_INTERVAL` | 10000 | **30000** |
| `STATUS_SEND_INTERVAL` | 60000 | **120000** |
| HTTPS sensores | 30 s | **off** (bridge + telemetry MQTT 30 s) |

---

## 6. Fases de implementação (revisadas)

### FASE 0 — Broker ✅ / hardening ⬜

**Entregue:** Mosquitto autenticado, ACL lab.  
**Falta (0b — gate antes do bridge):**

- [ ] IP estático
- [ ] Firewall IPv4 1883
- [ ] User MQTT por `device_id`
- [ ] Rotacionar senha
- [ ] Teste pub/sub externo (MQTTX ou SSH)

**Rollback:** parar Mosquitto; ESP só HTTPS.

---

### FASE 1 — Bridge heartbeat → Supabase

**Entregue:** 📄 spec + `.env.example`.  
**Implementar:**

- [ ] `/opt/hidrowave-bridge/` Node.js
- [ ] Subscribe `hidrowave/+/heartbeat`, `+/status`
- [ ] PATCH `device_status` (`last_seen`, `is_online`)
- [ ] systemd + logs sem vazar keys

**Teste:** pub manual → dashboard online.  
**Gate:** Fase 0b completa.

---

### FASE 2 — ESP publica MQTT (HTTPS paralelo, soak curto)

**Implementar:**

- [ ] `MqttClient.*`, `ENABLE_MQTT`, `platformio.ini` PubSubClient
- [ ] Heartbeat 30 s, telemetry 60 s, LWT
- [ ] **Não** mudar intervalos HTTPS ainda

**KPI 48 h:** heap ≥ baseline; mensagens no broker; bridge atualiza `last_seen`.

---

### FASE 2b — Viável (−600 REST/h mínimo)

**Implementar:**

- [ ] Intervalos doc 09 (poll 30 s, sync 30 s, status 120 s)
- [ ] Desligar HTTPS sensores; telemetry MQTT 30 s
- [ ] Throttle bridge 30 s

**KPI:** REST/h &lt; 600; gráficos sem buracos &gt; 2 min.

---

### FASE 3 — Comandos MQTT + idempotência

**Implementar:**

- [ ] Subscribe `command`; parser → mesma lógica `relay_commands`
- [ ] **Dedup por `id`** (NVS ring buffer ~32 ids)
- [ ] Rejeitar payload sem `id`
- [ ] Bridge: INSERT `relay_commands` → publish MQTT
- [ ] Ack continua HTTPS `markCommandCompleted`

**Fluxo:**

```
App → INSERT relay_commands → Bridge → MQTT command → ESP (dedup) → executa → ACK HTTPS → relay/state MQTT
```

**KPI:** 100 comandos, 0 duplicata; latência &lt; 2 s.

---

### FASE 3b — Gaps de confiabilidade (paralelo ou logo após 3)

**Implementar:**

- [ ] `esp_reset_reason()` no boot → campo em `device_status` (migration SQL)
- [ ] Política pós-reboot relés documentada e codada:
  - **Recomendado hidro:** safe mode (relés OFF) + sync Supabase
  - **Alternativa:** restore NVS se checksum OK e idade &lt; X min
- [ ] `loadMasterRelayStatesFromNVS()` integrado em `begin()` conforme política
- [ ] ESP verifica `reboot_count` remoto (reboot API) — alinhar STATUS doc

---

### FASE 4 — Ideal (drift + on-change)

**Implementar:**

- [ ] `relay/state` MQTT on-change
- [ ] Sync HTTPS 60 s (reconciliação drift)
- [ ] `STATUS_SEND_INTERVAL` 300 s; relay_master on-change
- [ ] Opcional: Supabase Realtime no front

**Drift:** dessincronia ESP ↔ Supabase ↔ UI; sync 60 s **corrige**, não substitui MQTT on-change.

---

### FASE 5 — Operação

- Runbook: novo ESP (ACL + Supabase + secrets)
- Monitoramento Mosquitto + bridge
- MQTTS se exigência formal
- Doc operacional senhas

---

## 7. Gaps prioritários (backlog ordenado)

| P | Gap | Impacto | Fase |
|---|-----|---------|------|
| P0 | Bridge não existe | MQTT não reflete na UI | 1 |
| P0 | Dedup por `id` | Relé duplicado com MQTT+poll | 3 |
| P1 | NVS load não restaura relés | Estado errado pós-reboot | 3b |
| P1 | `esp_reset_reason` ausente | Debug produção | 3b |
| P1 | User MQTT compartilhado | Segurança broker | 0b |
| P2 | Reboot remoto ESP não reage | Feature incompleta | 3b |
| P2 | IP estático / firewall | Campo instável | 0b |
| P3 | MQTTS | Sniffing WAN | 5 |
| P3 | Realtime front | UX (não integridade) | 4 |

---

## 8. Comparação solicitada (MQTT vs MQTT+WS no ESP)

| Critério | Híbrido MQTT+Supabase (nosso plano) | MQTT+WebSocket no ESP |
|----------|-------------------------------------|------------------------|
| Integridade comando | Alta (fila DB + dedup) | Igual ou pior (2 canais) |
| Consistência estado | DB + drift sync | Risco dupla verdade |
| Recovery reboot | NVS + Supabase | +estado WS |
| RAM ESP32 | Meta −65% REST | Pior |
| UI tempo real | Supabase Realtime | WS no chip (desnecessário) |

**Conclusão:** WebSocket no ESP **não** entra no plano. Realtime fica **browser ↔ Supabase**.

---

## 9. KPIs globais

| KPI | Alvo |
|-----|------|
| Heap livre mínimo | ≥ baseline pós-fase 2b |
| REST/h | &lt; 600 (2b), &lt; 350 (4) |
| Comando duplicado | 0 em teste 100× |
| Offline UI | &lt; 45 s após queda ESP |
| Bridge downtime | ESP operacional via poll |

---

## 10. Cronograma sugerido

| Semana | Fase | Entrega |
|--------|------|---------|
| 1 | 0b + 1 | Broker hardened + bridge heartbeat |
| 2 | 2 | ESP publish + soak |
| 3 | 2b | Intervalos + −600 REST/h |
| 4 | 3 + 3b | Comandos MQTT + dedup + reset reason |
| 5 | 4 + 5 | on-change + runbook |

---

## 11. Próximo passo imediato

1. Completar **Fase 0b** (checklist SSH/Lightsail).  
2. Implementar **Fase 1** (bridge) — pedir **“pode aplicar”** para código.  
3. **Não** iniciar firmware MQTT antes do bridge validar `last_seen`.

---

## Documentos do pacote

| Doc | Conteúdo |
|-----|----------|
| [01](./01_ARQUITETURA_E_DECISAO.md) | Decisão híbrida |
| [04](./04_MODELAGEM_TOPICOS_PAYLOADS.md) | Tópicos e JSON |
| [05](./05_SEGURANCA_PRODUCAO.md) | Segurança |
| [06](./06_BRIDGE_MQTT_SUPABASE.md) | Bridge spec |
| [07](./07_FIRMWARE_ESP32.md) | Firmware spec |
| [09](./09_INTERVALOS_REST_VS_MQTT.md) | Intervalos REST |
| [10](./10_REQUISITOS_CONFIABILIDADE.md) | R1–R9, dedup, reboot |

**Regra:** alteração de firmware/bridge só após **“pode aplicar”**.
