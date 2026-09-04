# Handoff — prioridades futuras (master ESP32)

**Fecha:** 2026-09-03  
**Ámbito:** firmware `ESP-HIDROWAVE-main` (master), camino MQTT/comandos, heap, safety de relés.  
**No tocar en esta carpeta:** código de producción. Solo planificación.

---

## 0. Contexto de bancada (serial reciente)

Firmware ya incluye: heap plan, guard batch (`lastAllRelaysReceivedMs`), `MQTT_COMMAND_PATH_STABLE_MS = 10s`, fase 1 ACK (no HTTPS si MQTT vivo / no HTTPS sin WiFi / ACK sin `relay_states` / no `relay/state` duplicado post-ACK).

**Idle ~500 s (terminal 8):**

| Métrica | Valor |
|---------|--------|
| heap | ~55 KB (20.9 %) |
| min | 42 940 |
| maxAlloc | 40 948 |
| sslBusy | 0 |
| mqtt | 1 |
| `ALL_RELAYS` | `mask=TTTTTTTT` (8 ON) |

Notas:

- Heap **no vuelve** a ~97 KB tras un episodio SSL/WiFi (fragmentación + objetos vivos).
- `mutex_timeout` sigue (~cada 30–60 s).
- Handshake `DEVICE-INFO` cada 30 s (nombre genérico `Slave-`).
- `WebServerTask` nullptr (esperado si el panel web no arranca).
- pH Modbus `0xE2` (sensor, no heap).

**Incidente 1940 (sesión anterior):** MQTT reset → cola ACK → HTTPS (`RPC COMPLETE`) → `BEACON_TIMEOUT` → ráfaga SSL. Fase 1b apunta a no repetir SSL sin WiFi.

---

## 1. Ya hecho (no reabrir sin evidencia)

1. Batch ESP-NOW + ACK `relay=255` → tickets individuales MQTT.
2. Guard batch: sin `ALL_RELAYS` recibido → comando individual (no máscara ciega).
3. Heap: skip cache JSON web, skip sync 60 s bajo heap/ACK, cooldown handshake, no reactivar HTTPS Supabase si MQTT estable, burst ACK.
4. `MQTT_COMMAND_PATH_STABLE_MS`: 60 s → **10 s**.
5. Fase 1 ACK MQTT:
   - MQTT conectado → no HTTPS (esperar stable).
   - WiFi caído → no HTTPS.
   - `command_ack` **sin** `relay_states[]` (estado por `relay/state` / `ALL_RELAYS`).
   - No `scheduleSlaveRelayStateMqtt` urgente tras ACK.

HTTPS ACK queda **último recurso**: MQTT desconectado **y** WiFi OK (cerrar ticket en Supabase). Opcional quitarlo del todo si se acepta esperar a que MQTT vuelva.

---

## 2. Prioridades futuras (orden)

### P0 — Safety real de dosificación (producto / planta)

**Problema:** `RELAY_SAFETY_TIMEOUT` **no se usa**. Dos `#define` distintos (5 min ms vs 2 h s). Un `on` instantáneo **no se apaga solo**. `SafetyWatchdog.h` existe pero **no está enganchado** (`main.cpp` comenta `watchdog.feed()`).

**Importancia:** peri/ácido atascados (loop Hydro bloqueado, ACK `off` perdido, máquina EC/pH colgada) → sobre-dosificación. Luces 24 h no son el mismo riesgo.

**Diseño (no es el macro actual):**

- Safety **detrás** del acionamiento: el comando manda ON+duración; un deadline independiente fuerza OFF.
- **No** una sola constante global.
- Dosificación: `expectedMs = ml/q + margen`; hard cap ~30–60 s.
- Recirc/drenaje: cap en minutos.
- Manual sin timer: decisión de producto (dejar o cap UI).

**¿Task?** Hoy **no**. Para “loop atascado”, el check **no** puede vivir solo en el mismo loop que se trabó.

| Capa | Rol |
|------|-----|
| Slave | Todo dose con `duration` (nunca ON forever). |
| Master HydroControl | Deadline de ciclo + abort. |
| Ticker/task corta (200–500 ms) **o** timer HW | OFF si `ON && now > deadline` en relés peri. |
| `esp_task_wdt` | Reset chip ~60 s; **no** apaga bombas a tiempo. |

**Fase 0 limpia (bajo riesgo):** borrar o unificar con `#ifndef` los dos `RELAY_SAFETY_TIMEOUT`. Cero cambio funcional.

Log sugerido: `[SAFETY] force OFF Rn reason=dose_deadline`.

---

### P1 — Contención mutex / `getAllTrustedSlaves`

**Síntoma:** `[SLAVE-LINK] event=mutex_timeout`.

**Causa:** copia de `std::vector<TrustedSlave>` bajo mutex; muchas llamadas (incl. `main.cpp` doble copia “slaveStillOnline”).

**Acción:** `getTrustedSlave(mac)` o iterar bajo lock corto; no copiar el vector entero.

**Impacto:** menos timeouts ESP-NOW, no +40 KB heap.

---

### P2 — HTTPS ACK como último recurso (opcional quitar)

Si MQTT siempre reconecta en segundos, la cola de ACKs basta. Quitar HTTPS ACK elimina mbedTLS en el peor caso (WiFi OK, broker muerto minutos).

Riesgo: tickets `pending` hasta MQTT o `PENDING_CLOUD_ACK_MAX_ATTEMPTS`.

---

### P3 — `RelayCommand` sin `String` (refactor grande)

7 `String` en struct usada en ~20 `.cpp`. Enums (`action`, `mode`, `command_type`) + `char mac[18]`.

Ahorro heap ~cientos de bytes / ráfaga de comandos. **Después** de P0/P1.

---

### P4 — SKIP / no priorizar

| Ítem | Por qué skip |
|------|----------------|
| `buf[1024]` en callback MQTT RX | Stack, no heap. `ec/config` ~751 B; riesgo si crece. |
| Batch `command_ack` multi-id | Bridge + RPC + ESP. Publish ya es stack. |
| `push_back` sin `reserve` | Irrelevante con 1 slave. |
| Bajar `setBufferSize(2048)` MQTT | Config EC/PH + telemetría. |

---

## 3. No hacer

- Un `RELAY_SAFETY_TIMEOUT` único para luces y peris.
- Safety que viva **solo** en el loop de dosificación si el objetivo es “loop atascado”.
- Reactivar `SafetyWatchdog` tal cual (pensado master↔slave heartbeat / bombas si master offline en **slave**; no sustituye deadline de dose en master).
- Volver `MQTT_COMMAND_PATH_STABLE_MS` a 60 s (fuerza HTTPS en los primeros ACKs).

---

## 4. Validación sugerida (próximo flash)

1. Clic 1–2 relés: un `[MQTT] command_ack` por ticket; **un** `[MQTT] relay/state` por batch (no dos).
2. Sin `🔒 [RPC COMPLETE]` si MQTT está up.
3. Cortar WiFi a mitad de ACK: **no** ráfaga SSL.
4. Heap idle: anotar min / maxAlloc vs ~55 KB post-SSL.
5. Máscara batch vs clics (no `TTTTTTTT` si no se pidió).

---

## 5. Archivos clave

- `src/HydroSystemCore.cpp` — ACK MQTT, batch, flush HTTPS.
- `include/Config.h` — `MQTT_COMMAND_PATH_STABLE_MS`.
- `include/MASTER_CONFIG.h` / `include/ConfigUnified.h` — macros safety muertos.
- `include/SafetyWatchdog.h` — diseño no cableado.
- `include/RelayCommandBox.h` — `DEFAULT_MAX_DURATION = 86400` (timer, no safety ON).
- `infra/mqtt/bridge/index.js` — `command_ack` / `relay/state`.

---

## 6. Orden de trabajo recomendado

1. P0 fase 0: limpiar macros muertos.  
2. P0 fase 1: duration en slave + deadline HydroControl (peri).  
3. P1 mutex.  
4. P2 si aún hay SSL ACK en logs.  
5. P3 solo si fragmentación sigue siendo el límite.
