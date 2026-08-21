# Sensores EC, pH y flujo YFB5 — ESP-HIDROWAVE (RDWC)

Documentación de bancada para malha fechada Auto EC.  
Prototipo fuente: ESP-SENSORS. Sin válvula 3 vías — solenoides **slave ESP-NOW** para dreno y reposición.

---

## Cableado definitivo

| Sensor | Conexión ESP32 |
|--------|----------------|
| EC analógico | GPIO **33** (0–3.3 V ADC, escala 0–4400 µS/cm) |
| pH Modbus RS485 | RO=**34**, DI=**23**, DE+RE=**32** |
| YF-B5 flujo Hall | GPIO **4** + divisor **10k/20k** (amarillo ~5 V → ~3.3 V) |
| Nivel capacitivo | PCF8574 P0–P3 (L1 arriba → L4 abajo) |

GPIO **35** libre (diag EC). Ver [SENSOR_FLUJO_YFB5.md](./SENSOR_FLUJO_YFB5.md).

---

## EC analógico (`EcAnalogSensor`)

- Media de `EC_SAMPLES_PER_WINDOW` lecturas `analogReadMilliVolts` cada `EC_SAMPLE_INTERVAL_MS`.
- Calibración factor K en solución 1413 µS/cm: comando serial `EC CAL 1413`.
- Debug bancada: `lastSampleMilliVolts()`, `instantEcMicrosiemensPerCm()`.

---

## pH Modbus (`PhModbusSensor`)

- reg0 = temperatura solución (÷10 → °C), reg1 = pH (÷10).
- Discovery opcional: `PH_MODBUS_DISCOVERY=1` en `Config.h` (solo bancada).

---

## Malha fechada (firmware)

1. **Auto EC** detecta overshoot (`EC > SP + tolerance`) → `startEcDilution()`.
2. **Dreno:** solenoide slave dreno ON → YFB5 mide litros → OFF al objetivo o `EVT_NO_FLOW`.
3. **Fill:** solenoide slave fill ON → para en 1:1 litros YFB5 **o** sensor capacitivo L1 (`TANK_HIGH`).
4. **Recirculación** `tempo_recirculacao` → nuevo `checkAutoEC`.

Relés slave configurados en `ec_config_view` (`dilution_*_slave_mac`, `dilution_*_relay`).

---

## Verificación bancada

1. Serial EC GPIO33 + pH Modbus OK.
2. YFB5: log coherente con recipiente graduado.
3. Slave dreno → litros suben → OFF al objetivo.
4. Slave fill → para en YFB5 1:1 o capacitivo L1.
5. Auto EC ON, EC alta artificial → ciclo `auto` sin UI manual.

Ver también: [SENSOR_FLUJO_YFB5.md](./SENSOR_FLUJO_YFB5.md).
