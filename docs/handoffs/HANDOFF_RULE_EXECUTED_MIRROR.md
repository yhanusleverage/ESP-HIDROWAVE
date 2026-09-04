# HANDOFF — Espejo async DecisionEngine → relay_commands

## Objetivo

Registrar en `relay_commands` las ejecuciones del **DecisionEngine local** sin bloquear el relé ni usar HTTPS en el ESP32.

## Flujo (invertido vs manual UI)

```
DE local → Core actúa (ESP-NOW) → MQTT rule_executed → Bridge Railway → INSERT completed
```

Manual UI (sin cambios):

```
UI → INSERT pending → MQTT command → Core → command_ack → complete_relay_command
```

## Contrato MQTT `rule_executed` v1

Tópico: `hidrowave/{device_id}/rule_executed`

| Campo | Tipo | Requerido |
|-------|------|-----------|
| v | 1 | sí |
| device_id | string | sí |
| ts | uint32 | sí |
| event_id | string | sí (dedup bridge) |
| rule_id | string | sí |
| relay_index | 0–15 | sí |
| action | on/off/toggle | opcional |
| current_state | bool | sí |
| success | bool | default true |
| duration_s | int | opcional |
| slave_mac_address | MAC | opcional (slave) |

Bridge INSERT:

- `status`: `completed` | `failed`
- `created_by`: `decision_engine_local#{rule_id}`
- Sin fila `pending` previa

## Firmware

| Flag | Default | Descripción |
|------|---------|-------------|
| `RULE_EXECUTED_MIRROR_ENABLED` | 1 | Publica MQTT tras actuar |
| `RULE_EXECUTED_MIRROR_RATE_LIMIT_MS` | 2000 | Mínimo entre publishes globales |

Serial esperado: `[MQTT] rule_executed event=… rule=… relay=… ok=1`

## Bridge

- Subscribe: `hidrowave/+/rule_executed`
- Dedup: `{device_id}:{event_id}` — `RULE_EXECUTED_DEDUP_MS` (default 60s)
- Env: `RULE_EXECUTED_DEDUP_MS`

## Bancada

1. Flash firmware con `mqtt_enabled=1`
2. Regla `decision_rules` enabled en LittleFS / sync
3. Forzar condición → serial `[MQTT] rule_executed`
4. Bridge log: `[bridge] rule_executed INSERT relay=…`
5. Supabase: fila `created_by` like `decision_engine_local#%`, `status=completed`
6. Smoke sin regresión manual: clic UI → pending → completed (sin cambios)

### Checklist validación (post-implementación)

| Check | Criterio |
|-------|----------|
| Build | `platformio run -e esp32dev` SUCCESS |
| Manual batch | `[CMD mqtt]` + `[BATCH] flush` + `cloud_closed=true` (como ids 1723–1726) |
| Heap | `min` no peor que baseline ~87 KB en idle+MQTT |
| Espejo simulado | `node scripts/test-publish-rule-executed.js` → bridge INSERT |
| Idempotencia | republicar mismo `event_id` en &lt;60 s → `[bridge] rule_executed dedup` |
| MQTT offline | DE actúa; serial `[MQTT] rule_executed skipped (offline)` |

Script:

```bash
cd infra/mqtt/bridge
TEST_DEVICE_ID=ESP32_HIDRO_XXXXX TEST_RULE_ID=my_rule TEST_RELAY_INDEX=1 \
  node scripts/test-publish-rule-executed.js
```

## Fuera de alcance v1

- HTTPS INSERT desde Core
- Buffer NVS offline
- Fix pending 50s manual
- UI timeline DE
- DE → batch ESP-NOW compartido con manual (v2 si reglas en ráfaga)