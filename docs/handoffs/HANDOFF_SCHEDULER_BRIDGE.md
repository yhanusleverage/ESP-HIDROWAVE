# HANDOFF — Scheduler Bridge (cron 60s)

**Fecha:** 3 sep 2026  
**Estado:** implementado en repo — falta deploy Bridge + confirmar SQL en Supabase + deploy frontend

## Objetivo

Reloj de automatización **en el servidor** (Bridge Node Lightsail), no en el ESP32.

El Bridge evalúa `rule_schedules` cada 60 s y, si hay match de hora/día/semana de cultivo, dispara el **mismo camino que un comando manual de la UI**:

```
UI / API → INSERT rule_schedules
                ↓
Bridge cron 60s → match → decision_rules (acciones)
                ↓
INSERT relay_commands (pending, created_by=scheduler#rule_id)
                ↓
MQTT hidrowave/{device_id}/command
                ↓
Core ESP32 actúa → MQTT command_ack
                ↓
Bridge complete_relay_command (existente)
```

**No inventa transporte nuevo.** Firmware Core sin cambios para este feature.

---

## Por qué en el Bridge (no en el ESP)

| Opción | Problema |
|--------|----------|
| ESP32 + NTP | Reloj frágil; offline; DST; heap |
| pg_cron / Edge Function | Posible v2; hoy Bridge ya tiene MQTT + Supabase 24/7 |
| **Bridge `setInterval` 60s** | Reloj del servidor, sin deps nuevas (`Intl` nativo) |

Separación de responsabilidades:

- **`decision_rules`** = *qué* hacer (receta / acciones)
- **`rule_schedules`** = *cuándo* disparar esa regla

---

## Archivos

### Firmware / ESP
Ninguno (reutiliza MQTT `command` + `command_ack`).

### Bridge (`ESP-HIDROWAVE-main/infra/mqtt/bridge/`)

| Archivo | Cambio |
|---------|--------|
| `schedule-evaluator.js` | **nuevo** — evalúa schedules, INSERT + publish MQTT |
| `index.js` | import + `setInterval(evaluateSchedules, 60000)` |

### Frontend (`HIDROWAVE-main/`)

| Archivo | Cambio |
|---------|--------|
| `scripts/migrations/CREATE_RULE_SCHEDULES.sql` | **nuevo** — tabla + RLS |
| `src/app/api/automation/schedules/route.ts` | **nuevo** — CRUD GET/POST/PATCH/DELETE |
| `src/components/automacao/ScheduleEditor.tsx` | **nuevo** — UI tab Schedules |
| `src/components/automacao/AutomacaoTabs.tsx` | tab `schedules` |
| `src/app/automacao/AutomacaoPageClient.tsx` | render `ScheduleEditor` |

### Docs
- Este handoff: `docs/handoffs/HANDOFF_SCHEDULER_BRIDGE.md`
- Deploy Lightsail genérico: `docs/mqtt/HANDOFF_UPLOAD_LIGHTSAIL.md`

---

## Tabla `rule_schedules`

Script: `HIDROWAVE-main/scripts/migrations/CREATE_RULE_SCHEDULES.sql`

| Columna | Tipo | Notas |
|---------|------|--------|
| `id` | uuid PK | `gen_random_uuid()` |
| `device_id` | text FK → `device_status` | obligatorio |
| `rule_id` | text | FK lógica → `decision_rules.rule_id` |
| `enabled` | bool | default true |
| `schedule_type` | text | `daily` \| `weekly` \| `grow_week` |
| `time_start` | time | HH:MM — disparo en ese minuto |
| `time_end` | time | opcional (v1 no usa ventana activa) |
| `days_of_week` | int[] | 0=dom … 6=sáb; NULL = todos |
| `grow_week_index` | int | solo `grow_week` (0-based) |
| `timezone` | text | default `America/Sao_Paulo` |
| `last_triggered_at` | timestamptz | dedup &lt; ~90 s |
| `created_by` | text | UI: `web_interface` |

### Gotcha SQL (ya corregido)

PostgreSQL **no permite subquery** en `CHECK`. La versión inicial falló con:

```text
ERROR: 0A000: cannot use subquery in check constraint
```

CHECK válido (actual en el script):

```sql
days_of_week IS NULL
OR (
  cardinality(days_of_week) BETWEEN 1 AND 7
  AND days_of_week <@ ARRAY[0, 1, 2, 3, 4, 5, 6]
)
```

Si la tabla **no** se creó por el error, re-ejecutar el SQL completo (idempotente: `IF NOT EXISTS` + `DROP POLICY IF EXISTS`).

Verificar:

```sql
SELECT to_regclass('public.rule_schedules');
-- debe devolver 'rule_schedules'
```

---

## Lógica del evaluator (`schedule-evaluator.js`)

Cada tick (60 s):

1. `SELECT * FROM rule_schedules WHERE enabled = true`
2. Por cada fila:
   - Hora/minuto/día en `timezone` vía `Intl.DateTimeFormat` (Node ≥18, **sin luxon**)
   - Match si `time_start` = minuto actual
   - `weekly`: exige `days_of_week` contenga el día
   - `grow_week`: compara `grow_week_index` con semana actual desde `device_status.metadata.grow_start_date`
   - Skip si `last_triggered_at` &lt; 90 s (anti doble fire)
3. Match → carga `decision_rules` (`rule_id` + `device_id` + enabled)
4. Extrae `actions` del `rule_json` (`target_relay` / `relay_number`, `type`, `duration_ms`, `target_device_id`)
5. Por acción:
   - INSERT `relay_commands` pending, `created_by=scheduler#{rule_id}`, `command_type=rule`, `priority=50`
   - Publish MQTT QoS1 mismo schema v1 que el frontend
6. UPDATE `last_triggered_at`

Logs esperados:

```text
[scheduler] triggered rule=BOMBA_ENCHER device=ESP32_HIDRO_XXXXX actions=1 type=weekly
[scheduler] MQTT command published id=1234 relay=2 action=on → hidrowave/ESP32_HIDRO_XXXXX/command
```

Errores:

```text
[bridge] scheduler error: …
[scheduler] rule X not found or disabled …
[scheduler] INSERT relay_commands failed …
```

---

## UI

Ruta: **Automação → tab Schedules**

- Seleccionar regla existente (`decision_rules`)
- Tipo: Diario / Semanal / Semana cultivo
- Hora inicio (+ fin opcional)
- Días (checkboxes) si weekly
- Índice de semana si grow_week
- Toggle enabled / delete

API: `/api/automation/schedules`

| Método | Uso |
|--------|-----|
| GET `?device_id=` | listar |
| POST | crear |
| PATCH `{ id, ... }` | update / toggle |
| DELETE `?id=` | borrar |

---

## Deploy checklist

### 1. Supabase

1. Abrir SQL Editor
2. Pegar y ejecutar `CREATE_RULE_SCHEDULES.sql` (versión con `<@ ARRAY[...]`)
3. Confirmar `to_regclass('public.rule_schedules')`

### 2. Bridge Lightsail

PowerShell (**usar `$sshHost`, nunca `$host`**):

```powershell
$pem = "C:\Users\THANUS\Documents\Projects\LightsailDefaultKey-ca-central-1.pem"
$sshHost = "ubuntu@15.175.109.90"
$root = "C:\Users\THANUS\Documents\Projects\ESP-NEW_HOPE - FRONTEND - BACKUP -\ESP-HIDROWAVE-main"
$bridgeLocal = Join-Path $root "infra\mqtt\bridge"
$bridgeRemote = "/opt/hidrowave-bridge"

scp -i $pem "$bridgeLocal\index.js" "${sshHost}:${bridgeRemote}/index.js"
scp -i $pem "$bridgeLocal\schedule-evaluator.js" "${sshHost}:${bridgeRemote}/schedule-evaluator.js"

ssh -i $pem $sshHost "sudo systemctl restart hidrowave-bridge"
ssh -i $pem $sshHost "sudo journalctl -u hidrowave-bridge -n 40 --no-pager"
```

Sin `npm install` extra (sin luxon / node-cron).

ACL Mosquitto: **sin cambio** (user bridge ya publica `command`).

### 3. Frontend

Deploy normal (Vercel/etc.). Sin env nuevas.

---

## Bancada / validación

1. Tener al menos una `decision_rules` enabled con acciones de relé
2. Crear schedule en UI: p.ej. `daily` a la hora actual + 1–2 min
3. Esperar tick del Bridge (~60 s)
4. Logs Bridge: `[scheduler] triggered …` + MQTT publish
5. Supabase `relay_commands`: fila `pending` → `completed`, `created_by` like `scheduler#%`
6. Core: mismo comportamiento que comando manual (serial `[CMD mqtt]` / ack)
7. Smoke: comando manual UI sigue funcionando (sin regresión)
8. Dedup: no debe disparar 2 veces el mismo minuto (`last_triggered_at`)

### grow_week

Requiere en `device_status.metadata`:

```json
{ "grow_start_date": "2026-08-01" }
```

Semana = `floor((now - start) / 7 días)`.

---

## Relación con otros features

| Feature | Relación |
|---------|----------|
| Comando manual UI | Mismo path pending → MQTT → ack |
| `rule_executed` mirror (DE local) | Camino **invertido** (Core primero → INSERT completed). Scheduler **no** lo usa |
| DecisionEngine local (sensores) | Independiente; schedules son time-based en servidor |
| Relay router (`mayExecute`) | Core etiqueta owner `ScheduleP4` vía `triggered_by`/`created_by` `scheduler#…` — ver [HANDOFF_RELAY_ROUTER.md](./HANDOFF_RELAY_ROUTER.md) |

---

## Fuera de alcance v1

- Scheduler en ESP / NTP
- pg_cron / Edge Function
- Varios horarios en una sola fila (usar N filas)
- Repetición cada N horas
- Prioridad / resolución de conflictos entre schedules
- `time_end` como ventana activa (columna existe; evaluator dispara solo en `time_start`)

---

## Próximo paso operativo

1. Confirmar SQL OK en Supabase  
2. Subir `index.js` + `schedule-evaluator.js` a Lightsail y reiniciar servicio  
3. Deploy frontend  
4. Crear un schedule de prueba a +2 min y validar `relay_commands` + serial Core
