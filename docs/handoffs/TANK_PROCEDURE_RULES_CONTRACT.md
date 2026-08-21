# Contrato: procedimientos de tanque ↔ reglas / ScriptRunner

**12/jul/2026** · HydroWave Core

## Principio

| Capa | Responsabilidad |
|------|-----------------|
| **DecisionEngine + ScriptRunner** | Troca parcial, dreno completo, secuencia hidráulica (Atlas + L1–L4 + YFB5) |
| **HydroControl Auto EC / Auto pH** | Malha fechada (setpoint, dose, K, intervalos) — **no** como `actions` de regla |
| **Dilución FSM** | Sigue en `HydroControl` (telemetría `diluting_*`); el template `diluicao_hidraulica.json` es el **mismo contrato** documentado |

**Gate P1:** `priority >= 80` o `dilutionState != idle` → Auto EC/pH pausados hasta fin (`setTankProcedureActive` / dilución).

**EventBus:** no usar.

---

## Vocabulario de condiciones

| Sensor / campo | Valores | Uso |
|----------------|---------|-----|
| `level_1` … `level_4` | `alto` / `seco` (wet/dry labels) | Matriz reservorio |
| `water_level` | `alto` / `medio` / `baixo` / `vazio` | Agregado |
| `water_level_ok` | bool / 0–1 | Interlock |
| `ph`, `ec` | float | Solo **disparar** procedimiento, no dose fina |
| `session_liters` (opcode `wait_liters`) | float L | YFB5 tramo A→B |

---

## Vocabulario de actuación (scripts)

| Opcode / campo | Semántica |
|----------------|-----------|
| `relay_action` | ON/OFF Atlas o master; `target_device_id` o `role` |
| `role`: `drain` \| `fill` | Resuelve a MAC/relé de dilución ya configurados en Core |
| `wait_liters` | Reset sesión YFB5; avanza cuando `sessionLiters >= liters` |
| `wait_level` | Avanza cuando `level_N` cumple valor |
| `delay` | Espera fija (ms) |
| `while` + `body` | Bucle con condición |
| `recirc` | Recirc física; `seconds` o default `tempo_recirculacao` Auto EC |

**Prohibido** en scripts de tanque: dose pH/EC en relés PCF dosadores del Core.

---

## Reglas de producto (dilución / reposición)

1. **Dreno:** cierra por **litros** (YFB5), no por stall de “sin caudal”.
2. **Fill:** cierra por **nível alto** (L1); no por tiempo como éxito.
3. **Después de reposição OK:** `recirc` **siempre**.
4. Bancada sin PCF: `HIDRO_SIMULATE_WATER_LEVELS` + `DILUTION_FILL_SIM_HIGH_MS` solo para E2E.

---

## Templates (catálogo)

Ver [`data/procedure_templates/`](../../data/procedure_templates/):

| Archivo | ID regla | Priority |
|---------|----------|----------|
| `troca_parcial.json` | `proc_partial_change` | 90 |
| `dreno_completo.json` | `proc_full_drain` | 90 |
| `diluicao_hidraulica.json` | `proc_dilution_hydraulic` | 90 |

---

## Paridad FSM dilución ↔ template

Ver [DILUTION_FSM_SCRIPT_PARITY.md](./DILUTION_FSM_SCRIPT_PARITY.md).
