# 09 — Intervalos REST vs MQTT: tabela completa e explicação detalhada

Este documento define **valores exatos** (em ms) para cada timer do firmware e como eles evoluem fase a fase. Objetivo numérico: sair de **~1.560 REST/h** para **~310 REST/h** por ESP, mantendo segurança e UI utilizável.

**Arquivos afetados quando for codar:**
- `include/Config.h`
- `include/HydroSystemCore.h`
- `include/MqttClient.h` (novo — constantes MQTT)
- `src/HydroSystemCore.cpp` (condicionais `#if ENABLE_MQTT`)

**Regra:** alterações no firmware só após **“pode aplicar”**.

---

## Resumo visual por fase

| Fase | REST/h (aprox.) | MQTT msgs/h | Redução vs hoje | Objetivo |
|------|-----------------|-------------|-----------------|----------|
| **Hoje** | ~1.560 | 0 | — | Baseline |
| **2** (paralelo curto) | ~1.260 | ~180 | ~19% | Validar broker + bridge |
| **2b** (viável) | ~660 | ~200 | ~58% | MQTT compensa RAM e latência |
| **4** (ideal) | ~310 | ~220 | ~80% | ESP estável longo prazo |

---

## Tabela mestre — todos os intervalos

| # | Constante / operação | Arquivo hoje | **Hoje** | **Fase 2** | **Fase 2b** | **Fase 4** | REST/h hoje → 4 |
|---|----------------------|--------------|----------|------------|-------------|------------|-----------------|
| 1 | Sensores hidro + ambiente | `HydroSystemCore.h` `SENSOR_SEND_INTERVAL` | 30 s | 30 s | **OFF** | **OFF** | 240 → 0 |
| 2 | Heartbeat MQTT | `MqttClient.h` (novo) `MQTT_HEARTBEAT_INTERVAL_MS` | — | 30 s | 30 s | 30 s | — |
| 3 | Telemetria MQTT | `MqttClient.h` `MQTT_TELEMETRY_INTERVAL_MS` | — | 60 s | 30 s | 30 s | — |
| 4 | `device_status` | `STATUS_SEND_INTERVAL` | 60 s | 60 s | **120 s** | **300 s** | 60 → 12 |
| 5 | `relay_master` no ciclo status | `HydroSystemCore.cpp` | 60 s | 60 s | 120 s | **on-change** | 60 → ~5 |
| 6 | Sync relés unificado | `RELAY_STATES_SYNC_INTERVAL` | 10 s | 10 s | 30 s | **60 s** | 360 → 60 |
| 7 | Publish relay state MQTT | `MqttClient.h` | — | — | on-change | on-change | — |
| 8 | Poll `relay_commands` | `SUPABASE_CHECK_INTERVAL` + `COMMAND_POLL_INTERVAL_MS` | 5 s | 5 s | **30 s** | **30 s** | 720 → 120 |
| 9 | Comandos via MQTT subscribe | `MqttClient` | — | — | **ativo** | **ativo** | — |
| 10 | Poll `decision_rules` | `RULES_CHECK_INTERVAL` | 30 s | 30 s | 30 s | 30 s | 120 → 120 |
| 11 | EC config Supabase | `HydroSystemCore.cpp` (dinâmico) | 30 s default | igual | igual | igual | ~120 → ~120 |
| 12 | Throttle bridge telemetria | bridge `.env` | — | 60 s | 60 s | 30 s | — |

---

# Explicação detalhada de cada item

---

## 1. `SENSOR_SEND_INTERVAL` — envio HTTPS de sensores (hidro + ambiente)

**Onde:** `include/HydroSystemCore.h`  
**Valor hoje:** `30000` ms (30 segundos)  
**Função no código:** `HydroSystemCore::sendSensorDataToSupabase()` → chama:
- `supabase.sendEnvironmentData()` → tabela `environment_data`
- `supabase.sendHydroData()` → tabela `hydro_measurements`

**Quantas REST isso gera:**  
A cada 30 s são **2 POST HTTPS** (ambiente + hidro).  
Em 1 hora: 3600 ÷ 30 × 2 = **240 requisições/hora**.

**Por que pesa tanto:**  
Cada POST abre ciclo SSL (handshake ou reutilização de sessão), serializa JSON, espera resposta do Supabase (até 7 s de timeout configurado). São dados “gordos” comparados a um heartbeat MQTT de ~120 bytes.

### Fase 2 — manter 30 s (paralelo)
- **Valor:** `30000` (sem mudança)
- **Motivo:** você ainda não confia 100% no bridge. Se MQTT falhar, o histórico no Supabase continua vindo do HTTPS.
- **Custo:** +180 msgs MQTT/h **sem** reduzir REST — fase de soak **curta** (1–2 semanas).

### Fase 2b — desligar HTTPS de sensores
- **Valor:** `0` ou `#if ENABLE_MQTT` pula `sendSensorDataToSupabase()` quando bridge OK
- **Substituto:** tópico `hidrowave/{device_id}/telemetry` a cada 30 s; bridge faz INSERT em `hydro_measurements` e `environment_data`
- **Economia:** **240 REST/h**
- **Cuidado:** bridge precisa throttle e validação de campos (pH 0–14, etc.) igual ao `SupabaseClient::sendHydroData`

### Fase 4 — igual fase 2b
- Sensores **só** MQTT; HTTPS de sensores não volta salvo rollback (`ENABLE_MQTT=0`)

---

## 2. `MQTT_HEARTBEAT_INTERVAL_MS` — pulso “estou vivo” (MQTT)

**Onde:** `include/MqttClient.h` (novo)  
**Valor proposto:** `30000` ms (30 s) em todas as fases com MQTT ligado

**O que publica:**  
Tópico `hidrowave/{device_id}/heartbeat` com JSON pequeno:
`online`, `heap_free`, `rssi`, `uptime_s`, `device_id`, `v`

**Quantas mensagens:** 3600 ÷ 30 = **120 msgs/h** — payload ~100–150 bytes.

**Para que serve (com verbosidade):**  
O dashboard HIDROWAVE decide se o device está online olhando `device_status.last_seen`. Hoje esse campo só atualiza quando o ESP faz HTTPS a cada 60 s (ou quando outros fluxos escrevem). Com heartbeat MQTT a cada 30 s, o **bridge** faz PATCH em `last_seen` e `is_online=true` assim que recebe a mensagem. O usuário vê “online” mais cedo e, se reduzirmos depois o HTTPS de status, a UI **não piora**.

**Por que 30 s e não 10 s:**  
10 s = 360 msgs/h/device — desnecessário para estufa; 30 s já bate com a lógica do front (offline se &gt; 5 min). Menos tráfego, menos writes no Supabase.

**Não substitui sozinho:**  
Heartbeat **não** grava pH/TDS; só presença e saúde do chip.

---

## 3. `MQTT_TELEMETRY_INTERVAL_MS` — sensores via MQTT

**Onde:** `include/MqttClient.h` (novo)  
**Fase 2:** `60000` ms (60 s)  
**Fase 2b / 4:** `30000` ms (30 s) — alinhado ao que sensores tinham em HTTPS

**O que publica:**  
Tópico `hidrowave/{device_id}/telemetry` — pH, TDS, temperatura, umidade, `water_level_ok`.

**Mensagens/h:**  
- Fase 2 a 60 s: **60/h**  
- Fase 2b+ a 30 s: **120/h**

**Por que começar a 60 s na fase 2:**  
Enquanto HTTPS **também** envia sensores a cada 30 s, telemetria MQTT a 60 s evita duplicar inserts no Supabase a cada 30 s (bridge + HTTPS). Na fase 2 você valida conectividade MQTT sem flood no banco.

**Por que 30 s na 2b:**  
Quando HTTPS de sensores desliga, a UI e gráficos precisam da mesma resolução temporal que tinham antes. 30 s mantém paridade com `SENSOR_SEND_INTERVAL` antigo.

**Bridge:**  
`TELEMETRY_THROTTLE_MS=60000` na fase 2; na 2b pode baixar para `30000` quando HTTPS sensores estiver off.

---

## 4. `STATUS_SEND_INTERVAL` — HTTPS `device_status`

**Onde:** `include/HydroSystemCore.h`  
**Hoje:** `60000` ms (60 s)  
**Função:** `sendDeviceStatusToSupabase()` → `updateDeviceStatus()`  
Campos: `wifi_rssi`, `free_heap`, `uptime_seconds`, `is_online`, `firmware_version`, `ip_address`, `last_seen`

**REST/h hoje:** 3600 ÷ 60 = **60/h**

**Por que existe além do heartbeat:**  
`device_status` carrega metadados que heartbeat MQTT pode incluir aos poucos (RSSI, heap, firmware). Enquanto o bridge só processa “online + last_seen”, o PATCH HTTPS completo ainda é útil.

### Fase 2 — 60 s (igual)
Paralelo com heartbeat MQTT; redundância aceitável por pouco tempo.

### Fase 2b — 120 s (`120000` ms)
- **Economia parcial:** 60 → 30/h (−30 REST/h)
- **Lógica:** `last_seen` frequente vem do heartbeat (30 s); HTTPS status vira **backup de metadados** (IP, heap, versão firmware) a cada 2 min.

### Fase 4 — 300 s (`300000` ms = 5 min)
- **REST/h:** 12/h
- **Lógica:** online/offline rápido = MQTT LWT + heartbeat; status HTTPS vira “ficha técnica” periódica para suporte/debug, não para UI tempo real.

**Cuidado:** se desligar status HTTPS demais sem enriquecer heartbeat JSON, o painel pode perder RSSI/heap atualizados — incluir esses campos no heartbeat na fase 4.

---

## 5. `relay_master` no mesmo ciclo de status

**Onde:** `HydroSystemCore.cpp` dentro de `sendDeviceStatusToSupabase()`  
**Hoje:** a cada 60 s chama `updateRelayMaster()` — **+60 REST/h**

**O que faz:**  
Espelha array de estados dos 8–16 relés locais na tabela `relay_master` no Supabase. A UI lê daí para mostrar botões ON/OFF.

### Fase 2b — 120 s
Acompanha `STATUS_SEND_INTERVAL`; 30 REST/h.

### Fase 4 — **on-change** (event-driven)
- **REST/h:** ~5–20/h (só quando relé muda ou após comando)
- **MQTT:** publish em `hidrowave/{device_id}/relay/state` no mesmo evento
- **HTTPS backup:** POST `updateRelayMaster` após comando + sync lento (item 6)

**Por que on-change:**  
Relé não muda 360 vezes por hora; sync a cada 10 s era **polling de estado estável** — desperdício puro de SSL.

---

## 6. `RELAY_STATES_SYNC_INTERVAL` — sync master + slaves

**Onde:** `include/HydroSystemCore.h`  
**Hoje:** `10000` ms (10 s)  
**Função:** `syncAllRelayStatesToSupabase()` — master + todos slaves ESP-NOW

**REST/h hoje:** ~**360/h** (1 ciclo completo a cada 10 s)

**Por que era 10 s:**  
Garantir que UI e slaves não divergem do Supabase. Funciona, mas é o **segundo maior consumidor** de REST depois do poll de comandos.

### Fase 2 — 10 s (sem mudança)
Ainda não há MQTT de relay state; manter consistência.

### Fase 2b — 30 s (`30000` ms)
- **REST/h:** 120/h (−240)
- **Trade-off:** UI de slaves pode atrasar até 30 s se só depender deste sync — mitigado porque **após comando** já existe update imediato no código atual.

### Fase 4 — 60 s (`60000` ms) + on-change MQTT
- **REST/h:** 60/h backup
- **MQTT:** `relay/state` whenever state changes
- **Ideal:** bridge ou front usa Realtime; sync 60 s só reconcilia drift

---

## 7. Publish MQTT `relay/state` (on-change)

**Onde:** `MqttClient.cpp` — chamado de callbacks de relé / ACK ESP-NOW  
**Intervalo:** **não é timer** — evento

**Estimativa msgs/h:** 5–50 conforme automação (muito menor que 360 sync HTTPS)

**Verbosidade:**  
Quando o usuário liga bomba pelo app, a sequência desejada na fase 4 é:
1. UI → INSERT `relay_commands`
2. Bridge ou MQTT command → ESP executa
3. ESP publica `relay/state` com JSON atualizado
4. Bridge opcionalmente PATCH `relay_master`
5. Sync HTTPS 60 s só corrige se algo falhou no meio

Isso tira centenas de GET/POST “por rotina” e troca por mensagens **só quando o mundo muda**.

---

## 8. `SUPABASE_CHECK_INTERVAL` + `COMMAND_POLL_INTERVAL_MS` — poll de comandos

**Onde:**
- `HydroSystemCore.h` → `SUPABASE_CHECK_INTERVAL = 5000`
- `Config.h` → `COMMAND_POLL_INTERVAL_MS = 5000`
- `SupabaseClient.cpp` → respeita `COMMAND_POLL_INTERVAL_MS` dentro de `checkForCommands`

**Hoje:** a cada **5 s** o ESP faz GET em `relay_commands` (status pending).  
**REST/h:** 3600 ÷ 5 = **720/h** — **46% de todo REST do device**.

**Por que 5 s foi escolhido:**  
Latência aceitável para botão no app (~0,5–5 s). Caro em SSL e bateria/heat do ESP.

### Fase 2 — 5 s (mantém)
MQTT subscribe ainda não ativo; comportamento idêntico ao produção atual.

### Fase 2b — 30 s (`30000` ms) **+ MQTT command ativo**
- **REST/h backup:** 120/h (−**600**)
- **Canal principal:** subscribe `hidrowave/{device_id}/command` — latência &lt; 1 s
- **Por que 30 s backup:** se broker cair, em no máximo 30 s o ESP ainda pega fila Supabase; equilíbrio entre segurança e −600 REST/h (meta “viável”)

**Importante:** `SUPABASE_CHECK_INTERVAL` e `COMMAND_POLL_INTERVAL_MS` **devem ficar iguais** para não haver loop chamando check antes do mutex interno liberar.

### Fase 4 — 30 s (manter)
Não reduzir backup abaixo de 15 s sem monitoramento forte — ganho marginal (120→240/h) vs risco se MQTT silencioso.

**Fluxo comando fase 2b+ (verboso):**
1. Usuário clica “Ligar relé 3” no HIDROWAVE
2. Next.js insere linha em `relay_commands` (como hoje)
3. Bridge (webhook ou poll leve **no servidor**, não no ESP) publica JSON no tópico `command`
4. ESP recebe em ms, executa, ACK via HTTPS `markCommandCompleted` (event-driven — poucas REST)
5. A cada 30 s ESP ainda pergunta “tem pending?” — pega comando perdido se passo 3 falhou

---

## 9. Comandos MQTT subscribe (não é intervalo)

**Onde:** `MqttClient::loop()` + callback  
**Ativo a partir de:** Fase 2b

**Não gera REST no ESP** — substitui **720 polls vazios** por **1 conexão TCP persistente** que recebe push.

**Segurança:** deduplicar por `id` do comando; ignorar payload sem `id`; ACL só lê tópico `command` do próprio device.

---

## 10. `RULES_CHECK_INTERVAL` — decision engine / `decision_rules`

**Onde:** `HydroSystemCore.h`  
**Hoje e proposto:** `30000` ms (30 s) — **sem mudança em nenhuma fase**

**REST/h:** **120/h**

**Por que NÃO migrar para MQTT agora (explicação longa):**  
As regras de automação vivem no Supabase (`decision_rules`): condições, thresholds, múltiplos relés, prioridades. O ESP **puxa** essas regras, avalia localmente com sensores, e **gera** comandos. Isso é pull de configuração + lógica — padrão REST/poll. Colocar regras em tópico MQTT exigiria:
- republicar todas as regras a cada edição no front,
- versionamento de payload,
- risco de ESP executar regra stale,

O ganho seria ~120 REST/h (8% do total) — **pequeno** frente ao risco. **Manter 30 s HTTPS** até haver produto maduro de “rules sync”.

---

## 11. EC config — intervalo dinâmico Supabase

**Onde:** `HydroSystemCore.cpp` — usa `hydroControl.getAutoECInterval()` ou default 30 s  
**REST/h estimado:** ~**120/h** quando auto EC ativo

**Proposta:** **não alterar** nas fases MQTT iniciais.

**Motivo:** EC automático é função **crítica** (dosagem). Depende de config remota confiável; volume 120/h é aceitável comparado aos 720/h do poll de relés. Migrar EC config para MQTT é fase futura opcional.

---

## 12. Throttle do bridge — `TELEMETRY_THROTTLE_MS`

**Onde:** `infra/mqtt/bridge/.env` (servidor, não ESP)

| Fase | Valor | Motivo |
|------|-------|--------|
| 2 | 60000 ms | HTTPS ainda envia sensores a 30 s — evita 2 inserts/min duplicados |
| 2b | 30000 ms | HTTPS sensores off — paridade com telemetria |
| 4 | 30000 ms | Igual |

**Verbosidade:** throttle não é “atraso por preguiça” — é **proteção do Supabase** (cota, custo, RLS load) e coerência quando duas fontes alimentam a mesma tabela.

---

# Bloco proposto para `Config.h` (referência — não aplicado)

```c
// ===== MQTT (fases — ver docs/mqtt/09_INTERVALOS_REST_VS_MQTT.md) =====
#ifndef ENABLE_MQTT
#define ENABLE_MQTT 0
#endif

#if ENABLE_MQTT
  #define MQTT_HEARTBEAT_INTERVAL_MS     30000UL
  #define MQTT_TELEMETRY_INTERVAL_MS     30000UL   // fase 2: usar 60000
  #define MQTT_STATUS_HTTPS_INTERVAL_MS  120000UL  // fase 2b+ backup device_status
  #define MQTT_COMMAND_POLL_BACKUP_MS    30000UL   // substitui 5000 quando MQTT cmds ON
  #define MQTT_RELAY_SYNC_INTERVAL_MS    30000UL   // fase 2b; fase 4: 60000
  #define MQTT_SENSOR_HTTPS_ENABLED      0         // fase 2b: desliga sendSensorDataToSupabase
#endif
```

---

# Bloco proposto para `HydroSystemCore.h` (valores condicionais)

```c
// Baseline sem MQTT (hoje)
static const unsigned long SENSOR_SEND_INTERVAL = 30000;
static const unsigned long STATUS_SEND_INTERVAL = 60000;
static const unsigned long RELAY_STATES_SYNC_INTERVAL = 10000;
static const unsigned long SUPABASE_CHECK_INTERVAL = 5000;
static const unsigned long RULES_CHECK_INTERVAL = 30000;

// Com ENABLE_MQTT fase 2b — exemplos:
// SENSOR_SEND_INTERVAL          → skip se MQTT_SENSOR_HTTPS_ENABLED=0
// STATUS_SEND_INTERVAL          → 120000
// RELAY_STATES_SYNC_INTERVAL    → 30000
// SUPABASE_CHECK_INTERVAL       → 30000 (igual COMMAND_POLL_INTERVAL_MS)
```

---

# Contagem REST/h consolidada (checklist numérico)

| Operação | Hoje | Fase 2 | Fase 2b | Fase 4 |
|----------|------|--------|---------|--------|
| Sensores HTTPS | 240 | 240 | **0** | **0** |
| device_status HTTPS | 60 | 60 | 30 | 12 |
| relay_master HTTPS | 60 | 60 | 30 | ~10 |
| Sync relés | 360 | 360 | 120 | 60 |
| Poll comandos | 720 | 720 | **120** | **120** |
| decision_rules | 120 | 120 | 120 | 120 |
| EC config | ~120 | ~120 | ~120 | ~120 |
| **Total REST** | **~1.680** | **~1.680** | **~540** | **~422** |

*(Totais arredondados; EC varia se auto EC off.)*

**Meta “viável” (−600/h):** atingida na **fase 2b** (−~1.140/h vs sensores+poll+sync+status).  
**Meta “ideal” (−1.000/h):** atingida na **fase 4**.

---

# Ordem de implementação recomendada

1. **Fase 2:** só adicionar constantes MQTT; **não** mudar timers HTTPS ainda (soak 1–2 semanas).
2. **Fase 2b:** aplicar num **único commit**:
   - `COMMAND_POLL_INTERVAL_MS` = 30000
   - `SUPABASE_CHECK_INTERVAL` = 30000
   - `RELAY_STATES_SYNC_INTERVAL` = 30000
   - `STATUS_SEND_INTERVAL` = 120000
   - desligar `sendSensorDataToSupabase` se `MQTT_SENSOR_HTTPS_ENABLED=0`
   - ligar MQTT command subscribe
3. **Fase 4:** relay on-change + status 300 s + sync 60 s.

---

# KPIs após mudar intervalos

Medir 48 h após cada fase:

| KPI | Esperado fase 2b |
|-----|------------------|
| REST/h (log contador) | &lt; 600 |
| Heap mínimo | ≥ baseline ou melhor |
| Latência botão relé | &lt; 2 s |
| Gráfico sensores | sem buracos &gt; 2 min |
| Comandos perdidos | 0 em teste 100 cliques |

---

# Documentos relacionados

- [03_PLANO_IMPLEMENTACAO_FASES.md](./03_PLANO_IMPLEMENTACAO_FASES.md)
- [04_MODELAGEM_TOPICOS_PAYLOADS.md](./04_MODELAGEM_TOPICOS_PAYLOADS.md)
- [07_FIRMWARE_ESP32.md](./07_FIRMWARE_ESP32.md)
