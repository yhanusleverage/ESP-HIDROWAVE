# pH Modbus RS485 — integración ESP-HIDROWAVE

Documento de referencia para desarrollo (IA/dev). Fuente de verdad del firmware integrado + cruce con banco ESP-SENSORS.

---

## Índice de archivos

| Rol | Ruta |
|-----|------|
| Driver Modbus pH | `include/PhModbusSensor.h`, `src/PhModbusSensor.cpp` |
| Integración sensores | `include/HydroControl.h`, `src/HydroControl.cpp` |
| Pines y flags | `include/Config.h` |
| EC ventana (~6 s) | `include/EcAnalogSensor.h`, `src/EcAnalogSensor.cpp` |
| Export MQTT | `src/HydroSystemCore.cpp`, `src/MqttClient.cpp` |
| Bridge Supabase | `infra/mqtt/bridge/index.js` |
| Banco original | `SENSORE NOVOS/ESP-SENSORS/ESP-SENSORS-main/` |
| HW MAX485 | `SENSORE NOVOS/.../docs/PROTOTIPO_TESTES_HW.md` |

---

## Mapa de pines (validado)

| Función | GPIO | Notas |
|---------|------|-------|
| EC analógico | **33** | `EC_SENSOR_ANALOG_PIN` / `TDS_PIN` |
| RS485 RO (RX) | **34** | `PH_RS485_RX_PIN` |
| RS485 DI (TX) | **23** | `PH_RS485_TX_PIN` — no usar para LevelSensor |
| RS485 DE+RE | **32** | `PH_RS485_DE_RE_PIN` — no usar para tanque legacy |
| DS18B20 fallback | **4** | `TEMP_PIN` si reg0 Modbus falla |
| pH analógico legado | 35 | `PH_PIN` — solo si `USE_PH_MODBUS_SENSOR=0` |
| Niveles L1–L4 | PCF8574 P0–P3 | futuro; hoy `HIDRO_SIMULATE_WATER_LEVELS=1` |

**Conflicto resuelto:** GPIO 32/23 reservados para RS485; `LevelSensor` GPIO desactivado en modo simulación.

---

## Mapa Modbus (transmisor pH)

| Parámetro | Valor |
|-----------|-------|
| Baud | 9600, 8N1, Serial2 |
| Esclavo | 1 (`PH_MODBUS_ADDR`) |
| Función | 0x03 Read Holding Registers |
| reg0 `0x0000` | Temperatura ×10 → °C (`/ PH_MODBUS_TEMP_SCALE`) |
| reg1 `0x0001` | pH ×10 → valor pH (`/ PH_MODBUS_SCALE`) |
| Resolución | 0,1 pH / 0,1 °C |

### Códigos error Modbus

| Código | Significado |
|--------|-------------|
| `0x00` | OK |
| `0xE0` | Sin respuesta válida (slave ID) |
| `0xE2` | Timeout |

### Validación banco

- Sin sonda: `ph_raw ~ 140` → pH ~ 14
- Con sonda en agua: pH ~ 7–9
- Si escala incorrecta (×100): revisar `PH_MODBUS_SCALE` (default 10.0)

---

## Flujo procedural

```
HydroControl::update() cada loop
  └─ updateSensors()
       ├─ ecSensor->tick()
       ├─ si consumeWindowReady() (~6 s):
       │    ├─ phModbusSensor->readPH()  → pH, phValid, lastPhValidMs
       │    ├─ lastTempC() reg0        → temperature, tempValid (pipeline agua)
       │    └─ ec ventana              → ec, tds, ecValid
       ├─ ecSensor->updateLiquidTemperatureC(temperature)
       └─ HIDRO_SIMULATE_WATER_LEVELS → tankLevelOk=true, L1-L4=true

HydroSystemCore::loop()
  └─ publishMqttTelemetry() cada 30 s → ph, temperature, water_level_ok, level_1..4
  └─ checkAutoPH() / checkAutoEC() — sin bloqueo por nivel si simulación activa
```

**Calibración pH:** en el transmisor RS485 (menú físico), no en ESP32.

---

## Flags de compilación

| Flag | Default | Efecto |
|------|---------|--------|
| `USE_PH_MODBUS_SENSOR` | 1 | Driver RS485 en lugar de `phSensor` ADC |
| `HIDRO_SIMULATE_WATER_LEVELS` | 1 | L1–L4 ON, `water_level_ok=true`, sin interlock nivel |
| `HIDRO_DEV_RELAX_SENSORS` | 1 | Telemetría parcial si sensor stale; métricas relajadas |

Producción futura: `HIDRO_SIMULATE_WATER_LEVELS=0` en `platformio.ini`.

---

## Pipeline temperatura (reg0 → telemetría)

1. `PhModbusSensor::readPH()` lee reg0+reg1 en bloque
2. `lastTempC()` → `HydroControl::temperature` + `tempValid`
3. `EcAnalogSensor::updateLiquidTemperatureC(temperature)` — compensación EC
4. MQTT `telemetry.temperature` vía `getTemperature()` / `getWaterTemp()`
5. Fallback: DS18B20 GPIO4 si reg0 inválido y aún no hay temp válida

---

## Exportación Supabase (sin cambios firmware bridge)

| Topic MQTT | Campo | Origen |
|------------|-------|--------|
| `hidrowave/{id}/telemetry` | `ph` | `HydroControl::pH` |
| | `temperature` | reg0 Modbus o DS18B20 |
| | `water_level_ok`, `level_1..4` | simulados o PCF8574 |
| `ph_metric`, `ph_dose`, `ph_operation` | — | Auto pH existente |

Bridge: `ph_raw` derivado de MQTT `ph`; clamp 0–14 en `ph_display_clamped`.

---

## Orden init (HydroControl::begin)

1. OneWire + DallasTemperature (fallback temp)
2. `phModbusSensor->begin()` — Serial2 + DE/RE
3. `EcAnalogSensor` ADC GPIO33
4. PCF8574 (relés; niveles simulados omiten LevelSensor GPIO)
5. NVS config EC/pH

Ver también `ORDEM_PROCEDURAL_HYDROSYSTEMCORE.md`.

---

## Checklist post-flash

- [ ] Serial cada ~6 s: `[EC WINDOW]` + `[pH Modbus] reg0=… reg1=… temp=… ph=…`
- [ ] Sin errores Modbus 0xE0 persistentes
- [ ] `[LEVEL] HIDRO_SIMULATE_WATER_LEVELS=1` al boot
- [ ] Auto EC/pH no loguea "water_level_ok=false"
- [ ] MQTT telemetry incluye `ph`, `temperature`, niveles simulados

---

## Referencias

- `FLUJO_PROCEDURAL_EC_CONFIG.md` — ciclo Auto EC / poll config
- `infra/mqtt/bridge/README.md` — payloads telemetry y ph_metric
- `docs/handoffs/hydraulics/LEVEL_SENSORS_PCF8574_CAT6.md` — niveles reales futuros
