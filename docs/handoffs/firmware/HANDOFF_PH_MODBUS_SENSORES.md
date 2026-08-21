# Handoff: pH Modbus RS485 + EC GPIO33 — ESP-HIDROWAVE

**Fecha:** 2025-06-21  
**Estado:** Implementación firmware completada — validación en banco/hardware con éxito reportado.  
**Origen banco:** `SENSORE NOVOS/ESP-SENSORS/ESP-SENSORS-main/`  
**Destino producción:** `ESP-HIDROWAVE-main/`

---

## Resumen ejecutivo

Se integró el driver **PhModbusSensor** (RS485/MAX485) en el firmware principal, alineado al flujo procedural de **EcAnalogSensor** (ventana ~6 s). La temperatura del transmisor pH (**reg0 Modbus**) alimenta el mismo pipeline que antes usaba solo DS18B20 (`temperature`, telemetría MQTT, compensación EC). Los niveles de agua L1–L4 están **simulados ON** hasta instalar sondas en PCF8574.

**Felicitaciones al equipo:** los nuevos sensores (EC GPIO33 + pH Modbus) quedaron operativos en banco según feedback de la sesión.

---

## Lo que YA está hecho

### 1. Hardware / Config (`include/Config.h`)

| Señal | GPIO | Notas |
|-------|------|-------|
| EC analógico | **33** | `TDS_PIN` / `EC_SENSOR_ANALOG_PIN` |
| RS485 RO | **34** | `PH_RS485_RX_PIN` |
| RS485 DI | **23** | `PH_RS485_TX_PIN` |
| RS485 DE+RE | **32** | `PH_RS485_DE_RE_PIN` |
| DS18B20 fallback | **4** | `TEMP_PIN` si reg0 Modbus falla |
| pH ADC legado | 35 | `PH_PIN` — no usado con Modbus activo |

Flags definidos:

- `USE_PH_MODBUS_SENSOR=1`
- `HIDRO_SIMULATE_WATER_LEVELS=1`
- `HIDRO_DEV_RELAX_SENSORS=1` (ya existía)

### 2. Driver Modbus (nuevo)

| Archivo | Descripción |
|---------|-------------|
| `include/PhModbusSensor.h` | API: `begin()`, `readPH()`, getters temp/raw/error |
| `src/PhModbusSensor.cpp` | Serial2, DE/RE half-duplex, Modbus 0x03, reg0+reg1 |

Protocolo (validado ESP-SENSORS):

- 9600 8N1, esclavo 1
- reg0 → temp °C = raw/10
- reg1 → pH = raw/10

### 3. Integración HydroControl

| Cambio | Archivo |
|--------|---------|
| `phSensor` ADC reemplazado por `phModbusSensor` | `HydroControl.h/.cpp` |
| Lectura pH + temp en `ecSensor->consumeWindowReady()` (~6 s) | `updateSensors()` |
| `getWaterTemp()` implementado → retorna `temperature` | `HydroControl.cpp` |
| Miembros telemetría: `phValid`, `tempValid`, stale 12 s | `HydroControl.h` |

Flujo:

```
updateSensors() → ecSensor->tick()
  → consumeWindowReady() → phModbus.readPH()
  → pH/phValid + temperature/tempValid (reg0)
  → ec/teds/ecValid
```

### 4. Simulación niveles (sin hardware L1–L4)

Con `HIDRO_SIMULATE_WATER_LEVELS=1`:

- `tankLevelOk = true` siempre
- `isLevelWet(1..4)` → true
- `getWaterLevelAggregate()` → `"alto"`
- `isAutoDosingPausedByInterlock()` **no** bloquea por nivel (solo script tanque P1)
- `LevelSensor` GPIO **no** se inicializa (libera GPIO 23/32 para RS485)
- Telemetría MQTT exporta `water_level_ok`, `level_1..4`, `water_level`

### 5. Dependencias y build flags (`platformio.ini`)

- `4-20ma/ModbusMaster @ ^2.0.1` añadido
- `-D USE_PH_MODBUS_SENSOR=1`
- `-D HIDRO_SIMULATE_WATER_LEVELS=1`

### 6. Exportación cloud (sin cambios de schema)

La cadena existente ya funciona si `pH` y `temperature` entran por `HydroControl`:

- MQTT `hidrowave/{id}/telemetry` → bridge → `hydro_measurements`
- Auto pH: `ph_metric`, `ph_dose`, `ph_operation` (sin modificar)

### 7. Documentación

| Doc | Contenido |
|-----|-----------|
| `docs/firmware/PH_MODBUS_INTEGRATION.md` | Referencia técnica IA/dev (pines, Modbus, flujo, flags) |
| `ORDEM_PROCEDURAL_HYDROSYSTEMCORE.md` | Init actualizado: `phModbusSensor->begin()` |
| `SENSORE NOVOS/.../docs/PROTOTIPO_TESTES_HW.md` | MAX485, cableado, matriz T1–T6 |

---

## Lo que FALTA / pendiente

### Prioridad alta

| # | Tarea | Detalle |
|---|-------|---------|
| 1 | **Estabilizar build PlatformIO en Windows** | Errores `.o` missing / caché SCons corrupta. Acciones: borrar `.pio/build`, `pio run -j 1`, opcional mover a `C:\dev\ESP-HIDROWAVE-main`, añadir `build_jobs = 1` en `platformio.ini`. Un solo build a la vez. |
| 2 | **Validación MQTT end-to-end** | Confirmar en Supabase: `ph`, `ph_raw`, `temperature` desde reg0, niveles simulados en `device_status`. |
| 3 | **Checklist serial en placa** | Cada ~6 s: `[EC WINDOW]` + `[pH Modbus] reg0=… reg1=… temp=… ph=…`. Boot: `[LEVEL] HIDRO_SIMULATE_WATER_LEVELS=1`. |

### Prioridad media (producción)

| # | Tarea | Detalle |
|---|-------|---------|
| 4 | **Niveles reales PCF8574** | ✅ Cutover: `HIDRO_SIMULATE_WATER_LEVELS=0`; `DiscreteLevelBank` + PCF @ 0x20 NPN directo (sin PC817). Ver [`LEVEL_SENSORS_PCF8574_CAT6.md`](../hydraulics/LEVEL_SENSORS_PCF8574_CAT6.md). |
| 5 | **`PH_PROTOTYPE_RELAX_GUARDS=0`** | Cuando Auto pH esté calibrado en producción. |
| 6 | **`HIDRO_DEV_RELAX_SENSORS=0`** | Telemetría estricta (omitir campos stale). |
| 7 | **Comandos serial diagnóstico pH Modbus** | Paridad con `processEcSerialCommand()` — ej. `PH STATUS`, error 0xE0. |

### Prioridad baja / futuro

| # | Tarea | Detalle |
|---|-------|---------|
| 8 | Driver procedural `tick/consumeWindowReady` en PhModbus | Hoy `readPH()` síncrono en ventana EC (suficiente en banco). |
| 9 | `PH_MODBUS_DISCOVERY=1` | Barrido registros documentado en ESP-SENSORS, no implementado. |
| 10 | MAX3485 en producción | Sustituir MAX485 a 3,3 V marginal por transceiver 3,3 V nativo. |

---

## Archivos modificados / creados (índice rápido)

```
include/Config.h              — EC=33, RS485, flags
include/PhModbusSensor.h      — NUEVO
include/HydroControl.h        — PhModbusSensor, valid flags telemetría
src/PhModbusSensor.cpp        — NUEVO
src/HydroControl.cpp          — integración ventana EC, simulación niveles
platformio.ini                — ModbusMaster + build flags
docs/firmware/PH_MODBUS_INTEGRATION.md — NUEVO
docs/handoffs/firmware/HANDOFF_PH_MODBUS_SENSORES.md — ESTE ARCHIVO
ORDEM_PROCEDURAL_HYDROSYSTEMCORE.md — actualizado
```

**No se tocó:** `infra/mqtt/bridge/`, frontend, schema Supabase, `AdaptivePHController`.

---

## Flags de compilación (referencia)

| Flag | Valor actual | Cuándo cambiar |
|------|--------------|----------------|
| `USE_PH_MODBUS_SENSOR` | 1 | 0 solo si volver a pH ADC GPIO35 |
| `HIDRO_SIMULATE_WATER_LEVELS` | 0 (producción) | 1 solo si bancada sin sondas |
| `HIDRO_DEV_RELAX_SENSORS` | 1 | 0 en producción |
| `PH_PROTOTYPE_RELAX_GUARDS` | 1 | 0 en producción Auto pH |

---

## Orden de build recomendado (evitar errores linker)

1. Cerrar todos los terminales PlatformIO activos.
2. `Remove-Item -Recurse -Force .pio\build`
3. `platformio run -j 1` → esperar `[SUCCESS]` (~2–5 min).
4. Verificar `.pio\build\esp32dev\firmware.bin`.
5. `platformio run --target upload`.

---

## Próxima sesión sugerida

1. Añadir `build_jobs = 1` en `platformio.ini` y rebuild limpio.
2. Flash + monitor serial: validar pH y temp Modbus en loop.
3. Verificar telemetría MQTT en bridge logs (`ph_raw`, `temperature`).
4. Probar Auto pH con lectura válida (sin log "water_level_ok=false").
5. Planificar instalación sondas nivel → desactivar simulación.

---

## Contacto con documentación hermana

- EC procedural: `FLUJO_PROCEDURAL_EC_CONFIG.md`
- Niveles futuros: `docs/handoffs/hydraulics/LEVEL_SENSORS_PCF8574_CAT6.md`
- Banco sensores: `SENSORE NOVOS/ESP-SENSORS/ESP-SENSORS-main/docs/`
