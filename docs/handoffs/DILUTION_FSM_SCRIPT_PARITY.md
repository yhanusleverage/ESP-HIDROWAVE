# Paridad: diluição FSM ↔ template `diluicao_hidraulica`

**12/jul/2026** · Contrato de coherencia (FSM **no** migrada aún).

Fuente FSM: `HydroControl` (`startEcDilution` / `processDilution`).  
Fuente script: [`data/procedure_templates/diluicao_hidraulica.json`](../../data/procedure_templates/diluicao_hidraulica.json).

| Paso FSM | Estado / telemetría | Opcode / instrucción template | Criterio de avance |
|----------|---------------------|-------------------------------|--------------------|
| Arranque dreno | `diluting_draining` | `relay_action` role=`drain` ON | — |
| Medir salida | progreso YFB5 | `wait_liters` (target L) | `sessionLiters >= liters` |
| Cerrar dreno | → fill | `relay_action` role=`drain` OFF | — |
| Abrir reposición | `diluting_filling` | `relay_action` role=`fill` ON | — |
| Esperar alto | aguardando L1 | `wait_level` sensor=`level_1` value=`alto` | L1 wet (o SIM_HIGH en bancada) |
| Cerrar fill | — | `relay_action` role=`fill` OFF | — |
| Recirc pós-fill | `recirculating` | `recirc` (`seconds: 0` → `tempo_recirculacao`) | siempre tras fill OK |
| Fin | `idle` | fin de script → release gate P1 | — |

## Qué queda fuera del template (sigue solo en FSM)

- Cálculo de litros por overshoot EC (`EcDilutionController`).
- Disparo automático desde Auto EC (`startEcDilution`).
- Telemetría `ec_operation_state` / `dilutionProgressL` (scripts genéricos: log serial; campo UI futuro).
- `DILUTION_FILL_SIM_HIGH_MS` (solo simulación bancada).

## Criterio de migración (otra entrega)

1. Opcodes `wait_liters` / `wait_level` / `recirc` + roles OK en bancada.
2. L1 reales (sin `HIDRO_SIMULATE_WATER_LEVELS`) o matriz documentada.
3. Checklist anterior 1:1 en E2E con `proc_dilution_hydraulic` enabled.
4. Entonces retirar FSM dilución de HydroControl y disparar el script desde Auto EC.
