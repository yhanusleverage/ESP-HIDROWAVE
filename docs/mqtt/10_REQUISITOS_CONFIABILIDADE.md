# 10 — Requisitos de confiabilidade (R1–R9)

Complementa o [00_PLANO_MESTRE.md](./00_PLANO_MESTRE.md).  
**Prioridade do produto:** integridade, sync e recovery — não latência mínima.

---

## R1 — Garantia de entrega de comandos

**Requisito:** comando iniciado no app deve chegar ao ESP ou permanecer na fila até ser consumido.

| Camada | Mecanismo | Status |
|--------|-----------|--------|
| Persistência | INSERT `relay_commands` (Postgres) | ✅ |
| Entrega rápida | MQTT `command` QoS 1 | ⬜ fase 3 |
| Backup | Poll HTTPS 30 s (fase 2b) | ⬜ hoje 5 s |
| Bridge | Publica MQTT após INSERT | ⬜ |

**Aceite:** com broker down, comando executa em ≤ 30 s via poll.

---

## R2 — Idempotência (sem execução duplicada)

**Requisito:** mesmo `relay_commands.id` não aciona relé duas vezes.

**Por que dedup é obrigatório (não opcional):**

- QoS 1 **pode duplicar** na camada MQTT.
- QoS 2 **não** elimina duplicata entre MQTT **e** poll HTTPS.
- Latência baixa **não** dispensa dedup — dois caminhos podem entregar o mesmo `id` quase juntos.

**Implementação proposta (fase 3):**

```cpp
// Pseudocódigo
if (!payload.containsKey("id")) reject();
if (processedIds.contains(id)) { ackIfNeeded(); return; }
executeRelay(...);
processedIds.add(id);  // NVS ring buffer, ~32 entradas
markCommandCompleted(id, ...);
```

**Aceite:** 100 entregas duplicadas simuladas → 1 execução física.

**Status:** ⬜ gap — não existe ring buffer de ids hoje.

---

## R3 — Acknowledgement explícito

**Requisito:** Postgres registra `completed` / `failed` com timestamp.

| Item | Status |
|------|--------|
| `markCommandCompleted()` | ✅ `SupabaseClient.cpp` |
| `markCommandFailed()` | ✅ |
| Callback ESP-NOW → ack | ✅ `HydroSystemCore.cpp` |
| Ack via MQTT | ❌ não — ack **sempre** HTTPS/DB |

**Motivo:** auditoria e UI leem Supabase; MQTT não é banco.

---

## R4 — Detecção de offline / reboot abrupto

| Mecanismo | O que detecta | Status |
|-----------|---------------|--------|
| LWT MQTT `status` retain | Broker perde conexão TCP | ⬜ |
| Heartbeat 30 s | Device vivo | ⬜ |
| Bridge → `is_online` | UI | ⬜ |
| `reboot_count` incremento | Reboot ocorreu | 🟡 conta local ✅; remoto ⬜ |

**Aceite:** desenergizar ESP → `is_online=false` em &lt; 60 s (LWT + bridge).

---

## R5 — Causa do reboot

**Requisito:** distinguir watchdog, power-on, software reset, brownout.

**API ESP32:** `esp_reset_reason()` no boot.

**Proposta:**

```cpp
// boot → device_status ou heartbeat
doc["reset_reason"] = (int)esp_reset_reason();
doc["reset_reason_str"] = "TASK_WDT"; // mapa legível
```

**Status:** ⬜ gap — não implementado.  
**Fase:** 3b.  
**SQL:** coluna opcional `last_reset_reason TEXT` em `device_status`.

---

## R6 — Restauração de relés após reinicialização

**Estado hoje:**

- `saveMasterRelayStatesToNVS()` — ✅ chamado no sync
- `loadMasterRelayStatesFromNVS()` — 🟡 **carrega mas não aplica** (comentário linha ~1449 `HydroSystemCore.cpp`)

**Políticas possíveis (escolher uma):**

| Política | Comportamento | Recomendação hidro |
|----------|---------------|-------------------|
| **Safe mode** | Boot → todos relés OFF → sync Supabase | ✅ **default** |
| Restore NVS | Aplica cache se checksum OK e idade &lt; N min | Risco bomba ligada sozinha |
| Restore Supabase | Boot → GET `relay_master` → aplicar | +HTTPS no boot; mais lento |

**Aceite fase 3b:** após power loss, relés master OFF até sync confirmar; log claro da política.

**Status:** ⬜ gap de política + código.

---

## R7 — Sincronização e drift

**Drift:** ESP, Supabase e UI com estados diferentes (mensagem perdida, bridge down, etc.).

**Correção em camadas:**

1. **Preventivo:** on-change MQTT `relay/state` + ack HTTPS (fase 4)
2. **Reconciliação:** sync HTTPS 60 s (fase 4)
3. **Verdade:** Supabase para UI; ESP para hardware

**Aceite:** após falha simulada de publish state, sync 60 s corrige UI.

---

## R8 — Tolerância a falhas do broker

| Falha | Comportamento |
|-------|---------------|
| Mosquitto down | ESP poll HTTPS 30 s; sensores HTTPS se fase 2 |
| Bridge down | Heartbeat MQTT não atualiza UI; HTTPS status backup |
| WiFi flap | Reconnect MQTT backoff; LWT pode flicker online |

**Aceite:** broker off 10 min → comandos ainda funcionam via poll; telemetria HTTPS se configurado fallback.

---

## R9 — Validação de payload

| Regra | Ação |
|-------|------|
| JSON inválido | Descartar + log |
| Sem campo `id` em command | Rejeitar |
| `device_id` tópico ≠ JSON | Descartar (bridge e ESP) |
| `relay_index` fora 0–7 | Rejeitar |
| Valores sensor NaN | Omitir no telemetry (igual HTTPS) |

---

## Matriz QoS (decisão fechada)

| Tópico | QoS | Retain | Motivo |
|--------|-----|--------|--------|
| heartbeat | 0 | false | Próximo ciclo corrige |
| telemetry | 0 | false | Histórico no DB |
| status / LWT | 1 | true | Offline rápido |
| relay/state | 1 | false | on-change; DB é verdade |
| command | 1 | false | + dedup R2 |

**QoS 2:** não usar (PubSubClient, overhead, não remove necessidade R2).

---

## O que WebSocket **não** resolve

| Necessidade | WebSocket no ESP? | Solução no plano |
|-------------|-------------------|------------------|
| Fila auditável | Não | Postgres |
| Dedup cross-channel | Não | ESP `id` |
| Histórico sensores | Não | Supabase |
| UI push | Não no ESP | Realtime Supabase |
| Causa reboot | Não | `esp_reset_reason` |

---

## Checklist de aceite por fase

### Fase 3 (comandos)
- [ ] R1 poll backup 30 s
- [ ] R2 dedup 32 ids NVS
- [ ] R3 ack HTTPS
- [ ] R9 rejeita sem `id`

### Fase 3b (recovery)
- [ ] R5 reset reason no boot
- [ ] R6 política safe mode documentada e codada
- [ ] Reboot remoto ESP (se escopo)

### Fase 4 (sync)
- [ ] R7 sync 60 s + on-change
- [ ] Teste drift simulado
