# Handoff — Recirculação como tipagem → regra `fn_circulation`

**Fecha:** 2026-09-04 (atualizado)  
**Ámbito:** UI tipagem hidráulica + MQTT circ/config + gate Auto EC/pH

## Producto

1. Usuario elige **Bomba de circulação** (Atlas) en Procedimentos → tipagem.
2. Al **Guardar tipagem**:
   - upsert Supabase `decision_rules.rule_id = fn_circulation`
   - publish MQTT retained `hidrowave/{device_id}/circ/config`
3. Core al recibir MQTT:
   - `RelayCoordinator::setCirculationTarget` → NVS
   - upsert local `fn_circulation` en SPIFFS `/rules.json`
4. **No** usar `water_level_ok` como gate de la bomba.
5. Interlocks Auto EC/pH:
   - `water_level_ok` (Normal/Carrera) → volumen para **dosificar**
   - Relé de circulación **ON** (regla periódica **o** toggle) → mezcla para la malla
   - Si bomba tipada y OFF / slave sin estado → Auto EC/pH **pausado**

`tempo_recirculacao` sigue siendo solo homogeneización post-dosis.

## Código

| Pieza | Path |
|-------|------|
| Fix React setState-in-render | `HIDROWAVE-main/.../HydraulicRelaySetupPanel.tsx` |
| Upsert regla cloud | `lib/circulation-rule-from-hydraulic.ts` |
| MQTT retained publish | `lib/mqtt-circ-config.ts`, `lib/mqtt-circ-publish.ts` |
| Hook save tipagem | `lib/hydraulic-roles-server.ts` |
| Gate dose | `HydroControl::isAutoDosingPausedByInterlock` + `RelayCoordinator::isCirculationMixActiveForDosing` |
| MQTT subscribe/apply | `MqttClient` + `HydroSystemCore::applyCirculationConfigMqtt` |
| Sin demo R6 | `DecisionEngine::createDefaultRules` vacío |

## Sync reglas

- HTTPS poll `decision_rules` **sigue desactivado** (heap/SSL; MQTT-first).
- Camino actual para recirculación: **MQTT circ/config** (no sync genérico de todas las rules).

## Tipagem por função (UI)

Cada card **Função fixa** tem **Salvar tipagem**:
1. Grava o relé em `hydraulic_roles_json`
2. Upsert `decision_rules` `fn_*` com **enabled=false** (ação simples condition + relay_on)
3. Circulação também publica MQTT `circ/config` (NVS + regra local inativa)

| Role | rule_id | Condição |
|------|---------|----------|
| circulation_pump | fn_circulation | time_window (sempre) |
| fill_valve | fn_fill_valve | water_level ≠ alto |
| drain_valve | fn_drain_valve | water_level ≠ vazio |
| recharge_pump | fn_recharge_pump | water_level ≠ alto |

Se a regra já existia e o operador a ativou, a tipagem **preserva** `enabled`.
