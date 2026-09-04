# HANDOFF — Relay Router (`mayExecute` + RelayCoordinator)

**Fecha:** 2026-09-03  
**Ámbito:** firmware master `ESP-HIDROWAVE-main`  
**Estado:** contrato + implementación v1 (`mayExecute`)

---

## 1. Idea

Un **router de actuación** ligero: todas las entradas (UI/MQTT, Bridge scheduler, DecisionEngine, ScriptRunner, HMI) pasan por `RelayCoordinator`. El motor **decide**; el coordinator **autoriza y ejecuta**.

```
UI / API / Bridge scheduler  →  MQTT command  →  HydroSystemCore
Sensores / reglas locales    →  DecisionEngine / ScriptRunner
HMI UART                     →  actuate*
        ↓
RelayCoordinator.mayExecute  →  policy (sin I/O)
        ↓
requestActuation / requestMask  →  PCF local / ESP-NOW
        ↓
command_ack  |  rule_executed (espejo)
```

**No** reintroducir EventBus. Ver [GLOBAL_EVENT_BUS_OFF.md](./firmware/GLOBAL_EVENT_BUS_OFF.md).

---

## 2. Capas

| Capa | Responsabilidad | Dónde |
|------|-----------------|--------|
| **Política** | *Qué* hacer (reglas, scripts de tanque) | DecisionEngine + ScriptRunner + LittleFS `/rules.json` |
| **Cuándo** | Reloj / cron | Bridge `rule_schedules` ([HANDOFF_SCHEDULER_BRIDGE.md](./HANDOFF_SCHEDULER_BRIDGE.md)) |
| **Cómo** | Ownership, interlocks, ESP-NOW/local | `RelayCoordinator` |
| **Auditoría** | Cloud tickets | MQTT `command_ack` (cloud-first) / `rule_executed` (core-first) |

Heap: el DE no debe copiar el mundo cada tick — [HANDOFF_HEAP_DECISION_ENGINE.md](./2026-09-03-prioridades-futuras/HANDOFF_HEAP_DECISION_ENGINE.md) (fase A en paralelo; fuera de este handoff).

Procedimientos de tanque: [TANK_PROCEDURE_RULES_CONTRACT.md](./TANK_PROCEDURE_RULES_CONTRACT.md).

Espejo local→cloud: [HANDOFF_RULE_EXECUTED_MIRROR.md](./HANDOFF_RULE_EXECUTED_MIRROR.md).

---

## 3. `RelayOwner` (quién mueve relés)

| Owner | Origen típico | Notas |
|-------|---------------|--------|
| `Manual` | UI / MQTT `command_type=manual` | Puede encolar slave offline |
| `DecisionRule` | DE local / reglas cloud sin schedule | Fail-fast si slave offline |
| `ScheduleP4` | Bridge scheduler (`triggered_by` / `created_by` `scheduler#…`) | Mismo tubo MQTT que UI |
| `TankScriptP1` | ScriptRunner priority ≥ 80 | Gate P1 pausa Auto EC/pH |
| `AutoEcRecirc` / `AutoPhRecirc` | Post-dose recirc | Circulación con refcount |
| `AutoEcDilution` | Dilución hidráulica | Puede **bloquear** bits de válvula |
| `None` | Liberación / OFF interno | |

Precedencia práctica v1 (no es un árbol completo de prioridades):

- Dilución (`AutoEcDilution`) bloquea Manual sobre válvulas en hold.
- Circulación: OFF denegado si otro owner sostiene el target.
- Bits `blockedBits`: solo `AutoEcDilution` puede actuar sobre ellos.
- Manual **no** se deniega por slave offline en `mayExecute` (sigue cola ESP-NOW).
- Automation (`DecisionRule` / `ScheduleP4` / `TankScriptP1`) **sí** deniega slave offline (fail-fast + serial).

---

## 4. Matriz `mayExecute` (v1)

| Condición | Deny reason | Owners afectados |
|-----------|-------------|------------------|
| Bit bloqueado por dilución | `BlockedBit` | todos excepto `AutoEcDilution` |
| Manual ON sobre válvula en dilución | `DilutionHold` | `Manual` |
| OFF circulación por owner ≠ holder | `CirculationConflict` | cualquier ≠ holder / ≠ None |
| Slave unreachable | `SlaveOffline` | `DecisionRule`, `ScheduleP4`, `TankScriptP1` |
| `!waterLevelOk` (callback + flag) | `WaterInterlock` | preparado; **default off** |

Serial canónico:

```text
[COORD] deny owner=DecisionRule reason=SlaveOffline R3
```

---

## 5. Idempotencia por borde

| Borde | Clave | Evita |
|-------|-------|--------|
| Bridge schedule | `last_triggered_at` (~90 s) | doble fire mismo minuto |
| MQTT command | `relay_commands.id` + ACK | reintentos mal cerrados |
| `rule_executed` | `event_id` + dedup Bridge | republish del mismo acto |
| Script / gate P1 | un hold hasta fin de script | Auto EC/pH vs dreno |
| Coordinator | owner + blocked bits + circulación | dos dueños en el mismo actuador |

Evaluar reglas cada 2 s **no** es un bug si cooldown / gate / `mayExecute` bloquean la segunda acción.

---

## 6. Owners: señales canónicas

### Scheduler → `ScheduleP4`

Bridge publica MQTT con `command_type=rule`, `priority=50`, `triggered_by=scheduler#<rule_id>` (y DB `created_by=scheduler#…`).

Firmware: `resolveCommandOwner` trata `triggered_by` que empieza por `scheduler` como **ScheduleP4** (no depender del rango 20–40 de priority).

### Script tanque → `TankScriptP1`

`ScriptRunner` pasa `priority` del `ActiveScript` a `executeRelayAction`. Si `priority >= TANK_SCRIPT_PRIORITY_THRESHOLD` (80) → owner `TankScriptP1`.

---

## 7. Fuera de alcance (este handoff / PR)

- Heap fase A (`forEachTrustedSlave` migración total) — paralelo.
- Safety dose deadline (P0 prioridades futuras).
- Unificar bypasses legacy `main.cpp` / Auto dose directo a `HydroControl::setRelay`.
- Activar water interlock por defecto.
- Roadmap multi-PR completo.

---

## 8. Archivos clave

| Área | Path |
|------|------|
| Gate | `include/RelayCoordinator.h`, `src/RelayCoordinator.cpp` |
| MQTT / owners | `src/HydroSystemCore.cpp` (`resolveCommandOwner`, `processRelayCommand`) |
| DE / scripts | `src/DecisionEngine.cpp`, `include/ScriptRunner.h`, `src/ScriptRunner.cpp` |
| Integration | `src/DecisionEngineIntegration.cpp` (callbacks reachable / water) |

---

## 9. Checklist bancada

1. Toggle manual UI → `[COORD] … owner=Manual` + ACK completed.  
2. Comando `triggered_by=scheduler#…` → serial `owner=ScheduleP4`.  
3. Script P1 → `owner=TankScriptP1` + Auto EC/pH pausados.  
4. Slave offline + regla DE → `[COORD] deny … reason=SlaveOffline` (no éxito silencioso).  
5. Dilución activa + Manual en válvula → deny `DilutionHold`.  
6. Batch mask UI sin regresión.
