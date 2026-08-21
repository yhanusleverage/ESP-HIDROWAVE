# ESP32 Master — mapa de recursos y contención

**Alcance:** competencia por CPU, heap, stack, radio WiFi/ESP-NOW, I2C y IRQs en el firmware Master (`ESP-HIDROWAVE-main`).  
**No mide:** calibración K/ppl del YF-B5 ni validación EC post-dilución (física distinta).  
**No tocar:** lógica 4 levels PCF, pH/EC/temp Modbus (salvo leer métricas).

**Telemetría bancada:** `RESOURCE_SERIAL_DEBUG=1` → línea `[RES]` cada `RESOURCE_LOG_MS` (default 10 s).

**Flowmeter (2026-07-23):** OK provisional tras OneWire OFF + divisor — idle `F=0`/`raw=0`; soplar `dt_min` en ms y `rej=ok`.

---

## Topología de ejecución

```
loop() ~10 ms (Arduino / Core 1 típicamente)
  └─ HydroStateManager::loop()
       └─ HYDRO_ACTIVE / ADMIN → HydroSystemCore::loop()
             └─ hydroControl.loop() → HydroControl::update()
espNowTask @ Core 0, 50 ms, stack 8 KB, prio 2   (MASTER_MODE)
WebServerTask @ Core 1                            (solo ENABLE_LOCAL_ADMIN_HTTP=1)
AsyncTCP @ Core 1, stack 16 KB, prio 10           (mismo flag)
```

`src/ESPNowTask.cpp` está **excluido** del build (`platformio.ini` `build_src_filter`). La task viva es `espNowTask` en `src/main.cpp`.

---

## Inventario de componentes

| Componente | Archivo(s) | Cadencia | Recurso | Flags |
|------------|------------|----------|---------|-------|
| Arduino `loop` + WDT | `src/main.cpp` | ~100 Hz (`vTaskDelay(10)`) | CPU | — |
| `HydroStateManager` | `src/HydroStateManager.cpp` | cada loop | CPU bajo | — |
| `HydroSystemCore::loop` | `src/HydroSystemCore.cpp` | cada loop (activo) | CPU alto (orquesta red) | — |
| `HydroControl::update` | `src/HydroControl.cpp` | cada loop | CPU / I2C / ADC | — |
| LCD `updateDisplay` | `HydroControl.cpp` | **cada update (~10 ms)** | **I2C/CPU muy alto** | — |
| PCF niveles L1–L4 | `DiscreteLevelBank` / `pollDiscreteLevels` | 200 ms | I2C | `LEVEL_POLL_MS` |
| PCF relés | `HydroControl` setRelay | event + timers | I2C | — |
| EC analógico | `EcAnalogSensor.cpp` | sample 200 ms; ventana ~6 s | ADC/CPU | — |
| pH Modbus RS485 | `PhModbusSensor.cpp` | ~6 s (con ventana EC) | UART/CPU **bloqueante** | — |
| YF-B5 ISR | `WaterFlowSensor.cpp` | por pulso (~≤200 Hz) | **IRQ** | GPIO4 |
| Flow `tick` + filtro | `WaterFlowSensor` / bank | ventana 1 s | CPU bajo | `FLOW_*` |
| `[FLOW]` serial | `HydroControl::update` | ~1 Hz | CPU/UART | `FLOW_SERIAL_DEBUG` |
| Dilución FSM | `processDilution` | cada update si activa | CPU bajo–medio | — |
| Auto EC / Auto pH | `HydroControl` | gated | CPU | — |
| DecisionEngine | `HydroSystemCore` | 2 s | CPU | — |
| Rules Supabase | `HydroSystemCore` | 30 s | heap/SSL | — |
| `espNowTask` | `main.cpp` | 50 ms | **CPU+radio Core 0**, stack 8 KB | `MASTER_MODE` |
| MasterSlaveManager | `MasterSlaveManager.cpp` | status 5 s; retry 2 s | CPU+radio | heap gate |
| ESP-NOW callbacks | `ESPNowController` | event | IRQ/radio | canal WiFi |
| WiFi STA | WiFi / WiFiManager | permanente | **radio+heap** | — |
| MQTT PubSubClient | `MqttClient.cpp` | loop cada core; telem 30 s; HB 60 s | CPU+radio | `ENABLE_MQTT` |
| HTTPS Supabase | `SupabaseClient.cpp` | hydro 30/90 s; status 60/120 s; syncs varios | **heap SSL muy alto** | pool SSL |
| ObjectPool SSL/HTTP | `SSLClientPool` / `HTTPClientPool` | residente | heap | init `main` |
| Cache web JSON | `HydroSystemCore` | 5 s | heap | skip si heap bajo |
| Admin HTTP :80 | `WebServerTask` / Async | on-request | heap burst Core 1 | `ENABLE_LOCAL_ADMIN_HTTP` |
| Admin WebSocket | `AdminWebSocketServer` | push ≤1 min | heap | ADMIN_PANEL |
| `[RES]` telemetría | `ResourceTelemetry` | 10 s | CPU bajo | `RESOURCE_SERIAL_DEBUG` |

---

## Protocolo bancada A–D (checklist)

Pegar del Serial Monitor **solo** líneas `[RES]` (+ opcional `[FLOW]` / `[SYNC]`) por fase.

### Fase A — Baseline idle (30–60 s)

- Condiciones: WiFi OK, sin soplar, `dil=idle`, Auto EC/pH off.
- Anotar: `heap`, `min`, `maxAlloc`, `loop/s`, `loop_hwm`, `espnow_hwm`, `wifi`, `mqtt`, `slaves`, `sslBusy`.
- Criterio orientativo: `heap` ~estable (~90 KB), `min` no en espiral, `loop/s` ≥ ~15–20, `espnow_hwm` >> 0.

### Fase B — Carga HTTPS / SSL

- Esperar o forzar sync Supabase (`[SYNC]`, RPC, `device_status`).
- Mirar caída de `heap`/`maxAlloc`, `sslBusy=1`, `loop/s` abajo.

### Fase C — MQTT (`ENABLE_MQTT=1`)

- Connect + ≥30 s telemetría.
- Comparar heap / `loop/s` vs solo HTTPS. Si `mqtt=0` y `rc=-2` → red/broker, no solo heap.

### Fase D — ESP-NOW + flujo

- Slave online si posible; soplar YF-B5 o dilución corta.
- Ver `espnow_hwm`, `slaves`, `loop/s`; `[FLOW]` idle limpio / soplar `rej=ok` con `dt_min` en ms.

### Qué pegar por fase

```text
--- FASE A ---
[RES] ...
[RES] ...
--- FASE B ---
[RES] ...
[SYNC] ...
--- FASE C ---
[RES] ...
[MQTT] ...
--- FASE D ---
[RES] ...
[FLOW] ...
```

---

## Cómo leer `[RES]` (serial 115200)

```text
[RES] up=120s heap=91234 min=70112 maxAlloc=65536 loop/s≈28 loop_hwm=1340 espnow_hwm=6000 wifi=1 mqtt=0 slaves=0 sslBusy=1 dil=idle
```

| Campo | Significado |
|-------|-------------|
| `up` | Uptime segundos |
| `heap` | `ESP.getFreeHeap()` |
| `min` | `ESP.getMinFreeHeap()` (mínimo histórico desde boot) |
| `maxAlloc` | bloque contiguo máximo (`getMaxAllocHeap`) |
| `loop/s` | iteraciones de `HydroSystemCore::loop` en la ventana |
| `loop_hwm` | stack HWM del task que llama (words libres) |
| `espnow_hwm` | HWM de `ESPNowTask` (words) |
| `wifi` | `1` si `WL_CONNECTED` |
| `mqtt` | `1` si MQTT conectado |
| `slaves` | `getOnlineSlaveCount()` |
| `sslBusy` | `1` si hot path SSL/ACK ocupado |
| `dil` | `idle` / `drain` / `fill` / `recirc` |

---

## Resultados bancada (plantilla + datos 2026-07-23)

Fuente: Serial Master ~up 59–130 s (post OneWire OFF, flow OK).

| Escenario | heap | min | maxAlloc | loop/s | espnow_hwm | wifi/mqtt/slaves | sslBusy | Notas |
|-----------|------|-----|----------|--------|------------|------------------|---------|-------|
| A idle | ~92–93 KB | 44208 | ~45044 | **19–28** | ~6000–6700 | 1 / 0 / 0 típ. | 1 a menudo | Heap estable; MQTT fail `rc=-2` |
| B HTTPS sync | pico ~134 KB free tras sync; luego ~93 KB | 44208 | 45044 | cae si busy | OK | — | **1** | Contiguo `maxAlloc` apretado |
| C MQTT | — | — | — | — | — | mqtt=0 | — | No conecta broker (red), no medir carga útil aún |
| D flujo soplar | ~92 KB | 44208 | 45044 | ~20 | OK | — | 1 | `[FLOW]` `rej=ok`, `dt_min` ms; idle `total` congelado |

Rellenar filas nuevas al repetir A–D con slave online / MQTT OK.

---

## Ranking hotspots (evidencia actual) + acciones

| Pri | Componente | Evidencia | Acción ahora |
|-----|------------|-----------|--------------|
| 1 | **HTTPS / SSL pool** | `sslBusy=1` frecuente; `min≈44 KB`; `maxAlloc≈45 KB` | Mantener gates heap; no bajar umbrales; revisar intervalos sync si `min` sigue bajando. **No throttle LCD aún.** |
| 2 | **LCD I2C cada loop** | Sospecha teórica; en este run `loop/s` 19–28 (≥15) | **Watch only.** Throttle `updateDisplay` **solo si** idle A da `loop/s` &lt; 15 de forma estable. |
| 3 | **ESP-NOW Core 0** | `espnow_hwm` ~6k words (holgado) | Sin cambio. |
| 4 | **YF-B5 ISR + `[FLOW]`** | Idle limpio; soplar OK | Flow OK provisional; `FLOW_SERIAL_DEBUG=0` en prod para menos UART. |
| 5 | **Modbus pH** | `0xE0`/`0xE2` (sonda/bus) | Fuera de recursos máquina; no es contención heap. |
| 6 | **MQTT** | `rc=-2` timeout | Broker/red; no confundir con OOM. |

**No hacer (producto):** reabrir DS18/OneWire; tocar `LEVEL_SENSOR_PCF_PINS` / 4 levels; auto-K flujo por EC.

---

## Fase 2 (fuera de este handoff)

- CPU% por task (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`)
- Throttle de `updateDisplay` (si evidencia `loop/s`)
- Refactor de intervalos HTTPS/MQTT

---

## Relacionado

- [`sensors/SENSOR_FLUJO_YFB5.md`](../../sensors/SENSOR_FLUJO_YFB5.md)
- [`GPIO_PIN_MAP_CORE.md`](../GPIO_PIN_MAP_CORE.md)
- `include/Config.h` — `RESOURCE_SERIAL_DEBUG`, `FLOW_SERIAL_DEBUG`, `ENABLE_MQTT`
- `src/ResourceTelemetry.cpp`
