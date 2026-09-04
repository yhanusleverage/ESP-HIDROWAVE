# Handoff — Tipagem + Regras + MQTT híbrido

**Fecha:** 2026-09-04  
**Estado:** implementado en código (flash bancada: conectar master COM4)

## Producto (cerrado)

| Decisión | Valor |
|----------|--------|
| Macros `fn_*` al tipar | Nacen **inactivas** |
| Nombres Motor | i18n (`rule_json.i18n_key` + `automacao.fixedRules`) |
| Procedimentos | Bloques **invoke_rule** → `fn_*` |
| Sin tipagem circ | Auto EC/pH **bloqueado** |
| Circ tipada + OFF | Pausa + badge mezcla (como nivel) |
| Fill/drain activos | Script P1 (priority 85) → pausan Auto EC/pH |
| Catálogo | 4 roles |
| Sync | Upsert por `rule_id` + manifest ids+hash |

## Flujo

```
Tipagem (por card)
  → hydraulic_roles_json
  → decision_rules fn_* (inactiva)
  → MQTT rules/{rule_id} + manifest
  → [só circ] MQTT circ/config → NVS gate

Motor / API rules
  → MQTT rules upsert + manifest
  → Core SPIFFS / DecisionEngine
```

## Topics MQTT

| Topic | Uso |
|-------|-----|
| `hidrowave/{id}/circ/config` | Binding NVS solamente |
| `hidrowave/{id}/rules/{rule_id}` | Upsert/disable retained |
| `hidrowave/{id}/rules/manifest` | Lista `{rule_id, hash, enabled}` |

## Gate mezcla

`RelayCoordinator::getCirculationMixGate()`:
- `NotTyped` si no hay NVS circ
- `Inactive` si tipada y relé OFF/inválido
- `Ok` si tipada y ON

Telemetría/levels: `circulation_typed`, `circulation_mix_ok` → `device_status` (SQL: `scripts/ADD_CIRCULATION_MIX_INTERLOCK.sql`).

## Tipagem → decision_rules (RLS)

Escritura de `fn_*` usa, en orden:
1. `SUPABASE_SERVICE_ROLE_KEY` en `.env.local` / Railway (bypass RLS), **o**
2. `Authorization: Bearer` del usuario logado (panel envía JWT; políticas `authenticated` + `device_status.user_email`).

Sin service role ni Bearer, el insert falla con RLS (`42501`).

## rule_id canónicos (slug ≈ título)

| Rol | ID |
|-----|-----|
| circulation_pump | `fn_recirculacao_continua` |
| fill_valve | `fn_enchimento_ate_alto` |
| drain_valve | `fn_dreno_ate_vazio` |
| recharge_pump | `fn_recarga_ate_alto` |

Aliases viejos (`fn_circulation`, …) se migran al tipar de nuevo o con `HIDROWAVE-main/scripts/MIGRATE_FN_RULE_IDS.sql` + Resync ↻.

**Activación:** MQTT `op=disable` **ya no borra** la regla en el Core (solo `enabled=false`). Tipagem siempre hace **upsert** (aunque inactiva) para que Activar en el Motor funcione sin re-tipar.

**Parse MQTT (2026-09-04):** buffer 8k; `conditions:[]` vazio já não falha (fallback `condition` / TIME_WINDOW se há `actions`). Tipagem R0 = bomba; R6 no serial era só acionamento manual. Reflash + ativar macro + Resync.

## Archivos clave

- Firmware: `RelayCoordinator.*`, `HydroControl.*`, `HydroSystemCore` MQTT rules, `MqttClient`
- UI: `HydraulicRelaySetupPanel`, `MixInterlockBadge`, `ProcedureBuilderPanel` invoke
- Sync: `mqtt-rules-publish.ts`, `fixed-function-rule-from-hydraulic.ts`

## Validación bancada

1. **SQL** (Supabase): `ADD_CIRCULATION_MIX_INTERLOCK.sql` + `MIGRATE_FN_RULE_IDS.sql`.
2. **ACL Lightsail** (`circ/config` + `rules/#`): ver [HANDOFF_UPLOAD_LIGHTSAIL.md §2b](../mqtt/HANDOFF_UPLOAD_LIGHTSAIL.md) — script `patch-acl-rules-circ.sh`.
3. Flash master (firmware con disable soft + `fn_recirculacao_continua`).
4. Tipar circulação → ID `fn_recirculacao_continua` inactiva; serial `[MQTT] rules upsert … ok`.
5. Activar en Motor (badge) → Core recibe upsert `enabled=true` → bomba Atlas ON.
6. Encadenar: lista `Recirculação contínua — fn_recirculacao_continua`.
7. Manual rápido: canal independiente; si falla, revisar MQTT comando / ESP-NOW (no el DE).
8. Resync ↻ tras migrate SQL.
