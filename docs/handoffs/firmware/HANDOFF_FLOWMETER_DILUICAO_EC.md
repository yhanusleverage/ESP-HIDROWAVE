# Handoff: Fluxómetro + Diluição EC Modo A — ESP-HIDROWAVE

**Data:** 2025-06-21  
**Estado:** Implementação firmware + UI + bridge + SQL **concluída no código** — **calibração física no banco hidráulico pendente** (tarefa humana).  
**Repositório firmware:** `ESP-HIDROWAVE-main/`  
**Repositório frontend/DB:** `HIDROWAVE-main/`  
**Contexto:** Diluição por overshoot de EC — dreno parcial medido por fluxómetro + reposição de água (volume do tanque ~constante).

---

## Para outra IA — leia isto primeiro

Este documento descreve **toda a implementação relacionada ao fluxómetro** na feature **Diluição EC Modo A**. Use-o como fonte única para:

- continuar calibração no banco físico;
- depurar “dreno não para” / “sem pulsos”;
- adicionar segundo fluxómetro na reposição;
- melhorar debounce, ISR, ou telemetria de pulsos.

**Não editar:** `.cursor/plans/diluição_ec_modo_a_*.plan.md`

---

## 1. Arquitetura hidráulica (Modo A)

```
Tanque → Bomba recirc → Bifurcação
                          ├─ Recirc principal → Tanque
                          └─ Rama DRENO → [FLUXÓMETRO] → esgoto/recipiente
Entrada ÁGUA (reposição) ──────────────────────────────→ Tanque
```

| Elemento | Função |
|----------|--------|
| Relé **dreno** | Abre válvula na bifurcação dreno |
| **Fluxómetro** | Na **saída** do dreno (após válvula) — mede volume drenado |
| Relé **reposição** | Abre entrada de água limpa (EC ≈ 0) |
| Fase fill | **Por tempo** (`volume_drenado / dilution_fill_flow_lps`) — **não** usa fluxómetro hoje |

**Fórmula de volume alvo** (agua EC≈0):

```
V_agua = V_tanque × (1 − EC_SP / EC_actual)
```

Condição overshoot (simétrica à banda morta de nutrientes):

```
EC_actual > EC_SP + tolerance  →  iniciar diluição
```

---

## 2. Hardware

### 2.1 Sensor esperado

- **YF-S201** ou clone compatível (saída em pulsos, típico ~450 pulsos/L @ 1″ — **calibrar no banco**).
- Saída: pulso digital por volume (open-collector / push-pull conforme modelo).
- Alimentação: 5 V (ver datasheet; alguns aceitam 3.3 V no sinal com divisor).

### 2.2 Ligação ESP32 (default firmware)

| Sinal fluxómetro | GPIO ESP32 | Notas |
|------------------|------------|-------|
| Pulso (OUT) | **GPIO 26** | `FLOWMETER_PULSE_PIN` em `include/Config.h` |
| VCC | 5 V | Fonte estável |
| GND | GND comum | GND compartilhado com ESP32 |

**Firmware:** `INPUT_PULLUP` + interrupção em **FALLING** (`FlowmeterSensor::begin()`).

### 2.3 Pinagem — conflitos a verificar

Antes de soldar GPIO 26, confirmar no `Config.h` / esquemático do master que **GPIO26 não está usado** por relés, I2C, RS485 DE/RE, etc.

Outros GPIOs já usados no projeto (referência PH handoff):

| Função | GPIO |
|--------|------|
| EC analógico | 33 |
| pH RS485 RX | 34 |
| pH RS485 TX | 23 |
| pH RS485 DE/RE | 32 |
| DS18B20 | 4 |

### 2.4 Relés diluição

| Parâmetro | Default firmware | Config Supabase/UI |
|-----------|------------------|---------------------|
| Relé dreno | `-1` (não configurado) | `ec_config_view.dilution_drain_relay` (0–7) |
| Relé reposição | `-1` | `ec_config_view.dilution_fill_relay` (0–7) |

Relés são **master dosificadores 0–7** (mesmo mapa que nutrientes/pH).

---

## 3. Software — classe `FlowmeterSensor`

### 3.1 Ficheiros

| Ficheiro | Papel |
|----------|-------|
| `include/FlowmeterSensor.h` | API pública |
| `src/FlowmeterSensor.cpp` | ISR + contagem |
| `include/Config.h` | Defaults `FLOWMETER_*`, `DILUTION_*` |
| `src/HydroControl.cpp` | FSM diluição + `processDilution()` |
| `include/HydroControl.h` | `DilutionState`, getters progresso |

### 3.2 API

```cpp
class FlowmeterSensor {
public:
    explicit FlowmeterSensor(uint8_t pulsePin, float pulsesPerLiter);
    void begin();                    // pinMode + attachInterrupt FALLING
    void tick();                     // reservado (hoje vazio)
    void reset();                    // zera pulseCount_ com noInterrupts()
    float consumedLiters() const;    // pulseCount_ / pulsesPerLiter_
    uint32_t pulseCount() const;
    void setPulsesPerLiter(float ppl);
};
```

### 3.3 Implementação ISR (detalhe crítico)

- **Padrão singleton:** `static FlowmeterSensor* activeInstance_` — só **uma** instância suportada.
- ISR: `void IRAM_ATTR FlowmeterSensor::isrThunk()` incrementa `volatile uint32_t pulseCount_`.
- **Ordem correta do atributo:** `void IRAM_ATTR FlowmeterSensor::isrThunk()` — **não** `void FlowmeterSensor::IRAM_ATTR isrThunk()` (erro de compilação GCC).

### 3.4 O que `tick()` NÃO faz hoje

- Não há debounce por software no `tick()` — confia só no hardware + FALLING edge.
- Não há filtro de frequência máxima (proteção contra ruído EMI).
- **Melhoria sugerida para outra IA:** debounce 1–5 ms ou contagem por polling no `tick()` se houver falsos pulsos.

### 3.5 Inicialização

Em `HydroControl::begin()` (ou equivalente no construtor/init):

```cpp
flowmeterSensor = new FlowmeterSensor(FLOWMETER_PULSE_PIN, FLOWMETER_PULSES_PER_LITER);
flowmeterSensor->begin();
```

Log serial esperado:

```
[DILUTION] Fluxometro GPIO26 450 pulsos/L
```

---

## 4. Integração — máquina de estados diluição

### 4.1 Estados (`DilutionState`)

| Estado | Relés | Medição volume |
|--------|-------|----------------|
| `DILUTION_IDLE` | off | — |
| `DILUTION_DRAINING` | drain ON, fill OFF | **Fluxómetro** → `dilutionProgressL` |
| `DILUTION_FILLING` | drain OFF, fill ON | **Tempo** × `dilutionFillFlowLps` |
| `DILUTION_RECIRCULATING` | off | recirc pós-diluição (`tempo_recirculacao`) |

### 4.2 Início do dreno (`startEcDilution`)

1. Valida: volume ≥ 0.1 L, ≤ `dilutionMaxVolumeL`, relés configurados, tanque não baixo, EC válida, pH válido se `HIDRO_EC_REQUIRES_PH_MODBUS=1`.
2. `flowmeterSensor->reset()`.
3. Abre relé dreno; fecha relé fill.
4. `dilutionTargetL = volume`; `dilutionProgressL = 0`.
5. Inicia watchdog de pulsos: `dilutionLastPulseMs = now`.

### 4.3 Loop `processDilution()` — fase DRAINING

Chamado em `HydroControl::update()` a cada ciclo.

```cpp
dilutionProgressL = flowmeterSensor->consumedLiters();

targetReached = dilutionProgressL >= (dilutionTargetL - 0.05f);  // histerese 50 mL

stall = (now - dilutionLastPulseMs) > DILUTION_FLOWMETER_STALL_MS
        && dilutionProgressL < 0.05f;   // 30 s sem pulsos e quase zero litros

maxDrainMs = (dilutionTargetL / dilutionFillFlowLps) * 2000;     // fallback 2× tempo estimado
timeFallback = (now - dilutionDrainStartMs) > maxDrainMs;
```

**Decisões:**

| Condição | Ação |
|----------|------|
| `targetReached` | `finishDilutionDrainPhase()` → fase FILLING |
| `stall && !timeFallback` | **Abort** sequência (`Fluxometro sem pulsos`) |
| `timeFallback && progress < 0.05 L` | Assume alvo atingido por tempo (sem fluxómetro útil) |
| Caso contrário | Continua drenando |

### 4.4 Fase FILLING (sem fluxómetro)

- Duração: `dilutionFillDurationMs = (dilutionDrainMeasuredL / dilutionFillFlowLps) * 1000` (mín. 1 s).
- `dilutionProgressL` na UI = tempo × vazão (estimativa).
- Ao terminar: evento `ec_dilution_events` + MQTT `hidrowave/{id}/ec_dilution`.

### 4.5 Interlocks

- Não dosar nutrientes se `dilutionState != IDLE`.
- Não diluir se dosagem nutrientes ativa (`DOSING` / `RECIRCULATING`).
- `RelayOwner::AutoEcDilution` no `RelayCoordinator` para ownership de relés.

---

## 5. Parâmetros de configuração

### 5.1 Firmware (`include/Config.h`)

| Macro | Default | Descrição |
|-------|---------|-----------|
| `FLOWMETER_PULSE_PIN` | 26 | GPIO pulsos |
| `FLOWMETER_PULSES_PER_LITER` | 450.0 | Calibrar no banco |
| `DILUTION_FILL_FLOW_LPS` | 0.5 | L/s reposição (só fase fill) |
| `DILUTION_FLOWMETER_STALL_MS` | 30000 | Timeout sem pulsos → abort |
| `DILUTION_MAX_VOLUME_L_DEFAULT` | 50.0 | Teto por ciclo |
| `DILUTION_DRAIN_RELAY_DEFAULT` | -1 | Até configurar na UI |
| `DILUTION_FILL_RELAY_DEFAULT` | -1 | Até configurar na UI |

### 5.2 NVS (Preferences) — chaves em `HydroControl`

| Chave | Campo |
|-------|-------|
| `dil_auto` | auto diluição |
| `dil_drainRelay` | relé dreno |
| `dil_fillRelay` | relé fill |
| `dil_maxVol` | volume máximo |
| `dil_fillLps` | vazão fill |
| `dil_ppl` | pulsos/L |

### 5.3 Supabase (`ec_config_view`)

| Coluna | Tipo | Default |
|--------|------|---------|
| `dilution_auto_enabled` | boolean | false |
| `dilution_drain_relay` | int | -1 |
| `dilution_fill_relay` | int | -1 |
| `dilution_max_volume_l` | float | 50 |
| `flowmeter_pulses_per_liter` | float | 450 |
| `dilution_fill_flow_lps` | float | 0.5 |
| `volume` | float | reutilizado como **V_tanque** |

**Script SQL:** `HIDROWAVE-main/scripts/ADD_EC_DILUTION.sql`

### 5.4 Progresso em tempo real (`relay_master`)

| Coluna | Uso |
|--------|-----|
| `ec_operation_state` | `diluting_draining` \| `diluting_filling` |
| `ec_dilution_target_l` | Volume alvo (L) |
| `ec_dilution_progress_l` | Litros medidos no dreno |

---

## 6. Telemetria MQTT / eventos

### 6.1 Operação EC (`hidrowave/{device_id}/ec_operation`)

Payload inclui opcionalmente:

```json
{
  "state": "diluting_draining",
  "dilution_target_l": 25.0,
  "dilution_progress_l": 12.3,
  "remaining_sec": 45
}
```

### 6.2 Evento concluído (`hidrowave/{device_id}/ec_dilution`)

```json
{
  "sequence_id": "dil-...",
  "source": "auto|manual|web",
  "ec_before": 740,
  "ec_setpoint": 555,
  "volume_target_l": 25.0,
  "volume_measured_l": 24.8,
  "drain_duration_s": 120,
  "fill_duration_s": 50
}
```

Bridge: `ESP-HIDROWAVE-main/infra/mqtt/bridge/index.js` → `INSERT ec_dilution_events`.

### 6.3 Comando manual (UI → MQTT)

`POST /api/ec-controller/dilution-start` publica em `hidrowave/{id}/command`:

```json
{ "v": 1, "action": "ec_dilution_start", "volume_l": 12.5, "source": "web" }
```

Parser: `src/MqttCommandParser.cpp` → `parseMqttEcDilutionCommand()`.

---

## 7. Calibração no banco hidráulico (PASSO A PASSO)

### 7.1 Calibrar `flowmeter_pulses_per_liter`

**Importante:** ppl/K se calibran con **volumen real (balde)**. La EC post-dilución valida la fórmula de dilución (`volume_target_l` vs `volume_measured_l` + EC), **nunca** auto-escribe K del Hall.

1. Configurar relé dreno na UI (**Automação → Diluição EC**).
2. Abrir **só** válvula de dreno (manual ou comando de teste).
3. Drenar volume **medido** (ex.: balde graduado 10.0 L).
4. Ler pulsos no serial:
   - adicionar log temporário: `Serial.printf("pulses=%u\n", flowmeterSensor->pulseCount());`
   - ou usar contador no evento de fim de ciclo.
5. Calcular: `ppl = pulsos_totais / litros_reais`.
6. Gravar em `ec_config_view.flowmeter_pulses_per_liter` + **Salvar diluição**.
7. Repetir com 5 L e 20 L para validar linearidade.

**Exemplo:** 4520 pulsos / 10.0 L → **452 pulsos/L**.

### 7.2 Calibrar `dilution_fill_flow_lps`

1. Fechar dreno; abrir só relé reposição.
2. Cronometrar encher **1.0 L** (ou medir balde em N segundos).
3. `dilution_fill_flow_lps = litros / segundos` (ex.: 1 L em 2 s → 0.5 L/s).
4. Salvar na UI.

### 7.3 Validar ciclo completo

1. EC artificialmente alta (ou solução stock) → preview UI mostra ~X L.
2. Iniciar diluição manual com volume sugerido.
3. Verificar:
   - badge **Drenando** + barra `progress/target`;
   - paragem do dreno ao atingir target ± 0.05 L;
   - badge **Reponendo**;
   - recirc pós-diluição;
   - linha em `ec_dilution_events`.

### 7.4 Valores iniciais sugeridos (banco)

| Parâmetro | Valor |
|-----------|-------|
| `volume` (tanque) | 100 L |
| `dilution_max_volume_l` | 30 L |
| `tolerance` | 50 µS/cm |
| `flowmeter_pulses_per_liter` | calibrar |
| `dilution_fill_flow_lps` | calibrar |

---

## 8. Frontend (referência rápida)

| Ficheiro | Função |
|----------|--------|
| `src/lib/ec-dilution.ts` | Fórmulas `calcDrainVolumeL`, `needsDilution` |
| `src/components/EcDilutionSection.tsx` | UI colapsável em Automação |
| `src/components/EcDilutionPreviewCard.tsx` | Preview V_agua, erro SP−EC |
| `src/hooks/useEcDilutionConfig.ts` | Load/save config |
| `src/hooks/useEcDilutionState.ts` | Realtime `diluting_*` + progresso |
| `scripts/verify-ec-dilution-formula.js` | Teste fórmula: `node scripts/verify-ec-dilution-formula.js` |

---

## 9. Diagnóstico / troubleshooting

| Sintoma | Causa provável | Ação |
|---------|----------------|------|
| `Fluxometro sem pulsos — abort` | Cabo GPIO, 5 V, GND, sensor morto | Multímetro no OUT; ver pulsos com LED do sensor |
| Dreno não para | `ppl` errado (muito alto) | Recalibrar; ver `consumedLiters()` no serial |
| Para cedo | `ppl` baixo (conta demais) | Recalibrar |
| `Timeout dreno — estimativa por tempo` | Fluxómetro inútil mas timeout atingido | Trata como atingiu target; **não confiar** — corrigir HW |
| Progresso UI parado | Bridge/MQTT down | Ver `ec_dilution_progress_l` em `relay_master` |
| Diluição não inicia auto | `dilution_auto_enabled=false` ou EC dentro da banda | Ver `checkAutoEC()` overshoot branch |
| Relé não abre | `dilution_*_relay = -1` | Configurar na UI e salvar |

### Logs serial úteis

```
[DILUTION] Fluxometro GPIO26 450 pulsos/L
🤖 [AUTO DILUTION] overshoot → 25.00 L (dreno+reposição)
⚠️ [DILUTION] Fluxometro sem pulsos — abort
💧 [DILUTION] Dreno OK 24.80 L — repondo ~50 s
✅ [DILUTION] Reposição concluída
```

---

## 10. Limitações conhecidas / trabalho futuro

1. **Só um fluxómetro** — na saída do dreno. Fase fill é **por tempo**, não por volume medido.
2. **Uma instância ISR** — não suporta 2 fluxómetros sem refactor.
3. **`tick()` vazio** — sem debounce software.
4. **Fallback por tempo** no dreno se 0 pulsos após `maxDrainMs` — perigoso em produção; preferir falhar fechado.
5. **Fill sem sensor** — erro acumula se `dilution_fill_flow_lps` estiver errado.
6. **GPIO26 fixo** — mudar requer rebuild; ideal: configurável via NVS/Supabase.

### Melhorias sugeridas para próxima IA

- [ ] Fluxómetro na entrada de reposição (fechar loop volume entrada = volume saída).
- [ ] Debounce + `portENTER_CRITICAL` na ISR se houver bounce.
- [ ] Publicar `pulse_count` em telemetria debug MQTT.
- [ ] Modo “calibração assistida”: drenar N L com contagem automática de `ppl`.
- [ ] Remover fallback `timeFallback` em produção (`HIDRO_DEV_RELAX` flag).

---

## 11. Testes automatizados existentes

| Teste | Comando |
|-------|---------|
| Fórmula V_agua | `node HIDROWAVE-main/scripts/verify-ec-dilution-formula.js` |
| Build firmware | `cd ESP-HIDROWAVE-main && python -m platformio run -j 1` |

**Nota build Windows:** se falhar com `opening dependency file .pio/.../*.d: No such file or directory`, executar `pio run -t clean` e repetir; pode ser race do filesystem.

---

## 12. Índice de ficheiros (fluxómetro + diluição)

```
ESP-HIDROWAVE-main/
├── include/
│   ├── FlowmeterSensor.h
│   ├── EcDilutionController.h      # fórmulas overshoot/volume
│   ├── Config.h                    # FLOWMETER_*, DILUTION_*
│   ├── HydroControl.h              # DilutionState, flowmeterSensor*
│   ├── RelayCoordinator.h          # RelayOwner::AutoEcDilution
│   └── MqttCommandParser.h         # ec_dilution_start
├── src/
│   ├── FlowmeterSensor.cpp
│   ├── EcDilutionController.cpp
│   ├── HydroControl.cpp            # processDilution(), startEcDilution()
│   ├── HydroSystemCore.cpp         # sync MQTT/Supabase, config dilution
│   ├── MqttClient.cpp              # ec_operation + ec_dilution topics
│   └── MqttCommandParser.cpp
├── infra/mqtt/bridge/index.js      # diluting_* states, ec_dilution_events
└── docs/handoffs/firmware/
    └── HANDOFF_FLOWMETER_DILUICAO_EC.md   ← este ficheiro

HIDROWAVE-main/
├── scripts/ADD_EC_DILUTION.sql
├── scripts/verify-ec-dilution-formula.js
└── src/
    ├── lib/ec-dilution.ts
    ├── components/EcDilutionSection.tsx
    ├── components/EcDilutionPreviewCard.tsx
    ├── hooks/useEcDilutionConfig.ts
    ├── hooks/useEcDilutionState.ts
    └── app/api/ec-controller/dilution-start/route.ts
```

---

## 13. Prompt sugerido para colar noutra IA

```
Estou a calibrar/depurar o fluxómetro YF-S201 na diluição EC Modo A do projeto ESP-HIDROWAVE.
Lê o handoff: docs/handoffs/firmware/HANDOFF_FLOWMETER_DILUICAO_EC.md

Contexto: GPIO26, ISR FALLING, consumedLiters = pulses / ppl.
O dreno para quando progress >= target - 0.05 L; stall 30s sem pulsos aborta.
A reposição é por tempo (dilution_fill_flow_lps), sem fluxómetro.

Tarefa: [DESCREVER — ex.: calibrar ppl com 10L medidos / adicionar debounce / segundo fluxómetro no fill]
```

---

*Documento gerado para handoff entre sessões de IA e calibração em banco físico.*
