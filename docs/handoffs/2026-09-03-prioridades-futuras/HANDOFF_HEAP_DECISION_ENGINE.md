# Handoff — Heap + Decision Engine (presupuesto de memoria)

**Fecha:** 2026-09-03  
**Ámbito:** firmware master `ESP-HIDROWAVE-main`  
**Tipo:** propuesta / diseño (aún **no** implementado salvo fixes recientes de ACK/serial/retry)  
**Relacionado:** [HANDOFF_PRIORIDADES_FUTURAS.md](./HANDOFF_PRIORIDADES_FUTURAS.md), [ESP32_MASTER_RESOURCE_MAP.md](../firmware/ESP32_MASTER_RESOURCE_MAP.md), handoffs de procesos Decision Engine en `HIDROWAVE-main/docs/handoffs/processes/`

---

## 1. Idea central

El **Decision Engine** puede ser el cerebro de **política** (qué hacer: reglas, schedules, interlocks), pero **no debe ser el mayor consumidor de heap**.

Hoy el heap se va sobre todo en:

1. **Copias** de `TrustedSlave` vía `getAllTrustedSlaves()`
2. **`String` + `std::vector` anidados** en reglas / condiciones / acciones
3. **ArduinoJson** grande (parse de reglas, cache web)
4. **`String` en hot path** de relés (`RelayCommand`, retry queue)
5. HTTPS / TLS (ya mitigado; telemetría `sslBusy` ahora = TLS real)

**Principio:** el motor **decide** con un snapshot fijo; los coordinadores (RelayCoordinator, ESP-NOW, MQTT) **ejecutan** sin alloc en el tick.

```
Sensores / estado  →  snapshot fijo (struct, sin String)
DecisionEngine     →  evalúa reglas (RAM fija / sin alloc en tick)
RelayCoordinator   →  dueño único de actuadores
ESP-NOW / MQTT     →  ACK/batch sin alloc
```

---

## 2. Presupuesto de heap (meta)

Con idle reciente ~**94–97 KB** libres tras fixes MQTT/ACK (antes episodios SSL dejaban ~55 KB fragmentados):

| Zona | Meta | Rol |
|------|------|-----|
| **Reserva dura** | ≥ 60–80 KB libres / ≥ ~40 KB contiguos (`maxAlloc`) | Supervivencia MQTT + ESP-NOW + WDT |
| **Hot path** | Casi **0 alloc** | Relés, ACK, batch, sensores, loop Hydro |
| **Decision Engine** | RAM fija + JSON solo al **cargar** reglas | Decide; no copia el mundo cada 2 s |
| **Cloud** | MQTT primero; HTTPS excepcional | Ya casi el camino actual |

Si el Decision Engine “engloba todo” a nivel producto, a nivel memoria debe **orquestar**, no **copiar**.

---

## 3. Evidencia / dolores actuales

| Síntoma | Causa probable |
|---------|----------------|
| `mutex_timeout` (slave-link) | Muchos callers de `getAllTrustedSlaves()` / `getTrustedSlave` bajo el mismo `trustedSlavesMutex`; copia grande del vector |
| Fragmentación post-SSL | TLS + objetos vivos; heap no vuelve a idle alto |
| Tick de reglas caro | `DecisionRule` / `RuleCondition` / `RuleAction` llenos de `String`; `getAllTrustedSlaves()` dentro de acciones slave |
| Serial confuso (ya mejorado) | `relay=255` / cajas ACK — no es heap, pero dificultaba auditar |

**No mezclar en este handoff:** pH Modbus `0xE2`, AUTO EC/pH desactivado, `WebServerTask` nullptr (si no usás panel local).

---

## 4. Qué NO debe vivir en el motor

- Sync HTTPS / sync full de relés 60 s  
- Construcción de JSON de UI / cache web  
- Handshake `DEVICE-INFO` / spam serial  
- Lectura Modbus pH / ADC EC (otro dominio; el motor solo **lee** el snapshot)  
- Safety de dosificación (deadline peri) — capa aparte (ver P0 en prioridades futuras)

El motor dice **qué**; el **cómo** (radio, MQTT, dose) vive en coordinadores / HydroControl.

---

## 5. Plan por fases

## Fase A — parcialmente hecha (2026-09-03 noche)

Hot path de **comandos slave / MQTT** ya no usa `getAllTrustedSlaves()`:

| Sitio | Ahora |
|-------|--------|
| `processManualCommand` (MQTT → slave) | `getTrustedSlave(mac)` |
| Heartbeat `relay/state` | `forEach` → lista de MACs → publish fuera |
| `forceSlaveRelayMqttFullSync` / AUTO-SYNC | igual (MACs primero) |
| `updateRelaySlaveState` | `readSlaveRelaySnapshot` |
| `RelayCoordinator::getObservedState` | `getTrustedSlave` |
| Cache JSON web slaves | `forEachTrustedSlave` |

**Pendiente:** `WebServerManager`, muchos callers en `main.cpp` (CLI on_all).  

**Bonus:** `DecisionEngine` remoto ya migrado a `forEach` + copia local de MAC (antes: puntero a vector temporal = UB).

Serial de comandos MQTT: **sin cambio de estilo** esperado; menos `mutex_timeout`.

### Fix ACK-MASK state (2026-09-03 noche)

`completePendingAcksForEspNowCommand` ya **no** usa `relayStates[]` stale (estado *antes* del click).  
Cierra cloud con `expectedOn` (lo que pediste). `ALL_RELAYS` / `relay/state` siguen siendo la fuente del UI en vivo.

**Probar:** OFF → `command_ack state=0`; ON → `state=1`. Luego migrar mismo criterio al Decision Engine si hace falta.


### Fase B — Snapshot fijo para el motor

**Problema:** cada evaluación toca sensores / slaves / Strings de forma ad hoc.

**Propuesta:**

- Una vez por ciclo del motor (p.ej. cada 2 s): llenar `SystemSnapshot` (ph, ec, temps, levels, flags wifi/mqtt, máscaras de relés locales/slave).
- DecisionEngine **solo** lee ese struct.
- Cero `getAllTrustedSlaves()` dentro de `evaluate` / `executeActions`.

**Éxito:** tick del motor con alloc ~0 (salvo log excepcional).

### Fase C — Reglas sin `String` en runtime

**Problema:** `DecisionRule`, `RuleCondition`, `RuleAction` usan `String` + `vector` anidados (`DecisionEngine.h`). Existe además un `DecisionEngineLoop` paralelo con otro `DecisionRule` (duplicidad conceptual).

**Propuesta:**

- Carga: JSON → structs con `char[]` / enums / índices de sensor.
- Runtime: sin concatenar `String` en hot path.
- Unificar o deprecar `DecisionEngineLoop` vs `DecisionEngine` (una sola fuente de verdad).

**Éxito:** cargar N reglas no fragmenta; evaluar no toca heap.

### Fase D — Hot path relés sin `String`

**Problema:** `PendingRelayCommand` y `RelayCommand` aún usan `String` para `action` / `commandMode`.

**Propuesta:** enums + `char mode[…]`; máscara ya guardada como `uint8_t relayMask` (hecho 2026-09-03: retry ya no inventa `0xFF`).

**Éxito:** batch/retry/ACK sin churn de heap.

### Fase E — JSON acotado

- Buffer estático / pool pequeño para parse de reglas.
- Cache web JSON: ya se skipea con heap bajo; mantener.
- No crecer `DynamicJsonDocument` ad hoc en el tick del motor.

### Fase F — Observabilidad de presupuesto

- Seguir `[RES]` (`heap`, `maxAlloc`, `sslBusy` = TLS real).
- Alarmas serial si `heap < 60K` o `maxAlloc < 32K` sostenido.
- Opcional: contador de allocs en tick del motor (solo debug).

---

## 6. Orden recomendado de implementación

| Orden | Fase | Riesgo | Por qué primero |
|-------|------|--------|-----------------|
| 1 | **A** | Medio | Ya duele en bancada (`mutex_timeout`) |
| 2 | **B** | Bajo–medio | Encaja Decision Engine sin reescribir reglas |
| 3 | **D** | Medio | Hot path de relés (producto diario) |
| 4 | **C** | Alto (refactor) | Después de A/B para no mezclar frentes |
| 5 | **E** / **F** | Bajo | Soporte |

**No hacer en el mismo PR:** safety de dose (P0 producto) + refactor completo de reglas String.

---

## 7. Estado ya hecho (contexto 2026-09-03, no reabrir)

Útil para no confundir “heap plan” con este handoff:

- Retry batch guarda **máscara real** (`relayMask`); no reenvía `0xFF` por `action=on_all`.
- Serial: `MASK=0x..`, `[AUTO-SYNC]`, sin cajas `🎊`; `main.cpp` ya no pisa el callback ACK de HydroSystemCore.
- `sslBusy` en `[RES]` = `isSslTransportBusy()` (TLS), no “hay ACK pendiente”.
- ACK cloud con MQTT conectado → no HTTPS.
- Rate-limit de log `mutex_timeout` (síntoma sigue; causa = fase A).
- **Fase A callers (2026-09-03 noche):** `main.cpp`, `WebServerManager.cpp`, `HmiUartBridge.cpp`, internos MSM migrados a `forEachTrustedSlave` / `getTrustedSlave` / collect-then-act. `getAllTrustedSlaves()` queda solo como API deprecada (definición), sin callers en hot path.

---

## 8. Criterios de aceptación (cuando se implemente)

1. Idle estable: heap ~≥ 90 KB y `maxAlloc` no colapsa tras 10 min + ráfaga de relés.  
2. Tick DecisionEngine: sin `getAllTrustedSlaves()`; sin `String` nuevas en evaluate (fase B/C).  
3. Bajo carga: `mutex_timeout` casi ausente o solo al arranque.  
4. Reglas slave siguen actuando vía RelayCoordinator / batch ESP-NOW como hoy.  
5. No regresión: MQTT `command` / `command_ack` / batch mask.

---

## 9. Archivos clave a tocar (futuro)

| Área | Archivos |
|------|----------|
| Slaves sin copia | `MasterSlaveManager.h/.cpp`, callers en `HydroSystemCore.cpp`, `DecisionEngine.cpp`, `RelayCoordinator.cpp`, `WebServerManager.cpp`, `HmiUartBridge.cpp`, `main.cpp` |
| Snapshot motor | `DecisionEngine.h/.cpp`, `DecisionEngineIntegration.*`, posiblemente `HydroSystemCore::loop` |
| Reglas | `DecisionEngine.h`, loaders JSON; decidir destino de `DecisionEngineLoop.*` |
| Relés | `DataTypes` / `RelayCommand`, `PendingRelayCommand` (ya tiene `relayMask`) |

---

## 10. Mensaje para el siguiente agente

> Optimizar heap **alrededor** del Decision Engine como dueño de **política + presupuesto**, no como módulo que copia slaves y Strings cada tick. Empezar por **fase A** (`getAllTrustedSlaves` → forEach/lookup). Luego snapshot (**B**). No tocar pH Modbus ni AUTO EC/pH en este trabajo. Safety de dose es otro handoff (P0).
