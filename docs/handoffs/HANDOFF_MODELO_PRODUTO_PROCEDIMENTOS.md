# Handoff — Modelo de producto HydroWave (tipagem · Motor · procedimentos · Auto)

**Fecha:** 2026-09-08  
**Estado:** decisiones de producto + gaps hacia “totalmente profesional”  
**Ámbito:** HIDROWAVE UI + ESP-HIDROWAVE Core + MQTT  

---

## 1. Destino de producto (a dónde queremos llegar)

Sistema de cultivo donde cada capa tiene un trabajo claro:

| Capa | Qué es | Qué NO es |
|------|--------|-----------|
| **Tipagem** | Qué relé Atlas es cada función hidráulica | No es la receta ni el “cuándo” |
| **Motor / `fn_*`** | Piezas cortas activables | No es el procedimento largo |
| **Procedimento (script)** | Historia del tanque (orden + condiciones) | No sustituye tipagem |
| **`tempo_recirculacao`** | Homogeneización post-dosis Auto EC/pH | No es la bomba tipada de circulación |
| **Auto EC/pH** | Nutrición / corrección | Debe pausar si el tanque se mueve |
| **Controlo Manual** | Intervención humana | Bloqueado si regra/Auto manda el relé |

**Regla mental:** tipagem ≠ regras ≠ procedimentos ≠ tempo_recirculacao.

---

## 2. Roles hidráulicos (tipagem)

Cuatro roles tipados (relé slave Atlas):

| Rol | `rule_id` canónico | Rol de producto |
|-----|--------------------|-----------------|
| Circulación | `fn_recirculacao_continua` | **Base** — mezcla; la que más sentido tiene sola en el Motor |
| Enchimento | `fn_enchimento_ate_alto` | Tipagem sí; historia larga → script |
| Dreno | `fn_dreno_ate_vazio` | Tipagem sí; historia larga → script |
| Recarga | `fn_recarga_ate_alto` | Tipagem sí; historia larga → script |

**Decisión de producto (sesión):**

- No priorizar las `fn_*` de fill/drain/recarga como “atajos” del Motor.
- Tipagem de las 4 **sí** (el script necesita saber qué relé mover).
- En el Motor, lo que “vive sola” casi siempre: **recirculación**.

Flujo tipagem:

```
Card Função fixa → elegir relé → Salvar tipagem
  → hydraulic_roles_json
  → decision_rules fn_* (enabled=false)
  → MQTT rules/{rule_id} + manifest
  → [solo circ] MQTT circ/config → NVS gate mezcla
```

Activar en el Motor = el operador enciende la pieza. Tipar ≠ activar.

---

## 3. Niveles (alto / vacío)

Son **inicio y fin** del paso (condición de loop), no decoración:

- **Dreno:** mientras `water_level != vazio` → actúa; al llegar a vacío → OFF / siguiente paso.
- **Enchimento / recarga:** mientras `!= alto` (4/4) → actúa; al llegar a alto → OFF.

---

## 4. Procedimento típico (ejemplo de producto)

Historia: dreno (con recirc + válvula) → luego llenar a 4/4.

```
[toggle] Bloquear Auto EC/pH     ← 1er paso (explícito)
while (nivel ≠ vacío)
  recirc ON
  dreno ON
dreno OFF (+ recirc según diseño)
while (nivel ≠ alto)
  fill ON
fill OFF
→ al terminar el script, Core libera Auto solo
```

- El `while` **ya es el loop**; no hace falta “abrir otro loop” aparte.
- Una acción simple (`relay_on`) **no alcanza** para esa historia → hace falta **script**.
- Recirc durante el fill: decisión de diseño pendiente (ON solo en dreno vs también en fill).

---

## 5. Bloquear Auto (interlock procedural)

### UX

- Toggle arriba del procedimiento: **“Bloquear Auto EC/pH en este procedimiento”**.
- ON → instrução `block_auto` como primer paso.
- OFF → quita `block_auto`.
- Botón **Liberar Auto** en la barra de add: **removido** (redundante).
- Liberación: automática al **fin / delete / disable** del script en el Core.

### Firmware

- `block_auto` → `holdAutoGate` → `setTankProcedureActive(true)` (aunque priority &lt; 80).
- Fin del script / `removeByRuleId` / `clear` → `releaseProcedureGate`.
- Coexiste con gate por **priority ≥ 80** vía flag `procedureGateHeld` (no doble traba en el mismo script).

### Por qué no solo “modo carrera”

| | Bloquear Auto (script) | Nivel / carrera |
|--|------------------------|-----------------|
| Quién decide | Operador en el procedimento | Sensor / estado |
| Claridad | Visible en UI | A veces “magia” |
| Uso | Tanque en movimiento | Volumen válido para dose |

**Convivencia:** carrera/nivel = ¿puedo dosificar? · circ tipada ON = ¿hay mezcla? · block_auto = ¿hay procedimento de tanque?

Redundancia con P≥80: **sí, a propósito** (red de seguridad + declaración explícita).

---

## 6. Conflictos entre funciones

Propuesta de prioridad mental:

**dreno / fill (evento tanque) > recarga > recirculación (base)**

- Solo recirc ON → Auto puede dosificar (si nivel/mezcla OK).
- Dreno o enchimento ON → Auto **trabado**.
- Recirc + dreno a la vez → gana el evento de tanque (recirc no pelea).

---

## 7. Controlo Manual

Modelo: **regra activa ⇒ relé bloqueado** (y ciclo Auto EC/pH en circulación).  
**Estado:** validado en bancada como OK — no reabrir salvo regresión.

---

## 8. Sync / Resync

- Flujo normal: realtime + MQTT (optimista).
- **Resync ↻** = acción **rara** de soporte, no paso rutinario tras tipar.
- Gap premium pendiente: badge **“en Core / desfasado”** (UI vs ESP).

---

## 9. Gaps hacia “totalmente profesional”

| Gap | Notas |
|-----|--------|
| Verdad operativa (“qué ejecuta ahora”) | Header aún habla más de enabled/prioridad que de `rule_executed` |
| Safety dose hard OFF | P0 planta; aparcado “después” |
| Fill/drain/recarga E2E = nivel circ | Afilar / validar |
| Procedimento guiado cerrado | Plantilla dreno→fill + pasos en runtime |
| Confirmación aplicado en Core | Sync OK / desfasado |
| Higiene regras (basura tipo `ass`) | Refinar reglas de creación |
| Ops: health, OTA, CI firmware | Flota |
| Alarmas / override HMI | Campo sin cloud |
| Soak / tests repetibles | No solo bancada |

---

## 10. Prioridad sugerida (próximos cortes)

1. Procedimento dreno→fill **cerrado** (tipagem + toggle Auto + script).
2. **En ejecución ahora** (`rule_executed` / espejo en UI).
3. Safety dose (después, según decisión previa).

**Ops:** flash master con `block_auto` / `holdAutoGate` antes de validar en bancada.

---

## 11. Archivos clave (referencia)

| Pieza | Path |
|-------|------|
| Tipagem → fn_* | `HIDROWAVE-main/src/lib/fixed-function-rule-from-hydraulic.ts` |
| Lock manual | `HIDROWAVE-main/src/lib/manual-slave-relay-lock.ts` |
| Toggle Bloquear Auto | `HIDROWAVE-main/src/components/instruction-editors/BlockAutoProcedureToggle.tsx` |
| Add pasos script | `HIDROWAVE-main/src/components/instruction-editors/InstructionAddButtons.tsx` |
| ScriptRunner | `ESP-HIDROWAVE-main/src/ScriptRunner.cpp` |
| Interlock Auto | `ESP-HIDROWAVE-main/src/HydroControl.cpp` (`setTankProcedureActive`) |
| Tipagem MQTT | `ESP-HIDROWAVE-main/docs/handoffs/HANDOFF_TIPAGEM_REGRAS_MQTT.md` |

---

## 12. Checklist bancada (procedimento + Auto)

- [ ] Firmware flasheado con `block_auto`
- [ ] Tipados circ + dreno + fill
- [ ] Script: toggle Bloquear Auto ON
- [ ] while ≠ vacío → dreno (+ recirc si aplica)
- [ ] while ≠ alto → fill
- [ ] Durante script: Auto EC/pH **no** dose
- [ ] Al terminar: Auto **liberado**
- [ ] Relés del script bloqueados en Controlo Manual mientras enabled

---

*Documento de producto/sesión. No sustituye handoffs técnicos de MQTT/router/heap.*
