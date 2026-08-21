# Sensor de flujo YF-B5 — ESP-HIDROWAVE

Caudalímetro Hall en rama común (dreno + reposición). Driver: `WaterFlowSensor` (paridad ESP-SENSORS).

**Pin producción:** GPIO **4** (`FLOW_SENSOR_PIN`). Temp agua = Modbus pH (sin DS18).

**Arquitectura:** ISR `FALLING` + debounce 100 µs + filtro Hz en ventana 1 s. Volumen = `pulsos / 396 × K`. Sin task FreeRTOS ni mutex — `tick()` desde `HydroControl::update()`.

---

## Cableado (amarillo ~5 V / 0 V → divisor 10k / 20k)

```
YFB5 Rojo     → 5 V (GND común con ESP32)
YFB5 Negro    → GND

YFB5 Amarillo ── 10 kΩ ──●── GPIO4
                         │
                      20 kΩ
                         │
                        GND
```

- En HIGH: \(V_{GPIO} \approx 5 \times 20/(10+20) = 3.33\,\text{V}\) (seguro para ESP32).
- En LOW: ~0 V.
- **No** conectar 5 V directo al GPIO4.
- Divisor antiguo 2×4.8k (~2.5 V) queda **obsoleto**; usar **10k serie + 20k a GND**.

### Testeo en monitor (Core)

Con `FLOW_SERIAL_DEBUG=1` en `Config.h` (default bancada), cada ~1 s:

```text
[FLOW] F=… valid=1 lvl=1 pulses=… Q=… total=… L rej=ok|idle|lo|noise | dil=idle|drain|fill|recirc prog=a/b L
```

Criterio bancada: con agua `F` ~7–200, `valid=1`, `total` sube; **sin agua** `F≈0`, **`lvl=1`** (HIGH idle).

Si **`lvl=0`** y `F`/`total` suben sin soplar: pin flotando o mal cableado — el Core usa `INPUT_PULLUP` como `flowmeter/`; revisar divisor 10k/20k y GND común.

Con `FLOW_DEBUG=1` (bancada):

```text
[FLOW dbg] why=ok|idle|lo|noise raw=… deb=… dt_min=… us dt_max=… us
```

`dt` bueno ~5000–150000 µs; `dt_min < 1000` = ruido/EMI.

---

## Firmware (`Config.h`)

| Macro | Default | Descripción |
|-------|---------|-------------|
| `FLOW_SENSOR_PIN` | **4** | Entrada pulsos |
| `FLOW_HZ_PER_LPM` | 6.6 | F = 6.6 × Q (Hz, L/min) |
| `FLOW_PULSES_PER_LITER` | 396 | Datasheet (6.6×60) |
| `FLOW_CALIBRATION_FACTOR` | 1.0 | K inicial (L_real/L_leido) |
| `FLOW_ISR_DEBOUNCE_US` | 100 | Debounce ISR |
| `FLOW_FILTER_ENABLE` | 1 | Banda ~6.6–208 Hz |
| `FLOW_WINDOW_MS` | 1000 | Ventana de muestreo |
| `FLOW_SERIAL_DEBUG` | 1 | Log `[FLOW]` bancada |

---

## Dos calibraciones distintas (no mezclar)

| Qué | Cómo | Cuándo |
|-----|------|--------|
| **K / `flowmeter_pulses_per_liter`** | Pulsos ÷ litros **reales** (balde) | Setup hidráulico / UI |
| **¿Dilución OK?** | EC antes → drena L → EC después vs setpoint | Cada ciclo Auto EC |

`applyFlowCalibrationFromPpl` convierte ppl → `K = 396/ppl` y aplica a **todos** los litros medidos.

**No** auto-ajustar K con `EC_esperado` vs `EC_obtenido`: mezcla, dead volume, sonda y recirc son física distinta del Hall. Si el volumen medido ≠ target → revisar ppl; si el volumen fue correcto y EC no → revisar fórmula/tanque/recirc.

Telemetría post-dilución: `volume_target_l` vs `volume_measured_l` + EC (eventos) — solo diagnóstico, sin write automático de K.

---

## Integración dilución

- `processDilution()` usa `sessionLiters()` / `totalLiters()` del mismo driver.
- Stall sin caudal → `EVT_NO_FLOW` → abort (según fase).
- Fill: 1:1 litros medidos **o** `TANK_HIGH` (PCF8574 L4) **o** fallback tiempo.

---

## GPIO 35

Libre para `EC_ADC_CMP_PIN` / diagnóstico. Ya **no** comparte YFB5.
