# Mapa de pines — HydroWave Core (ESP32)

**Fuente de verdad del build:** [`include/Config.h`](../../include/Config.h).  
**No usar** [`ConfigUnified.h`](../../include/ConfigUnified.h) para confrontar PCB.

**23/jul/2026** — YF-B5 en **GPIO4** (divisor **10k/20k → ~3.3 V**); temp agua vía Modbus; GPIO35 libre.

---

## Temp de agua — sin bornes DS18

Temp agua = **pH Modbus** (`reg0`). GPIO4 **no** es DS18 en PCB: es **flujo**.

**OneWire/DS18 desactivado para siempre** (`HIDRO_ENABLE_DS18B20_FALLBACK=0`). No reactivar: GPIO4 solo YF-B5 (ISR).

| GPIO | Define | PCB |
|------|--------|-----|
| 4 | `TEMP_PIN` legacy / `FLOW_SENSOR_PIN` | **YFB5** |
| 25 | `WATER_TEMP_PIN` | **NC** |

---

## Bornes PCB sugeridos (Core)

| Bornes / J | Señal | GPIO / bus | Notas |
|------------|-------|------------|-------|
| J-LED | LED_STATUS | GPIO2 | Built-in / LED |
| J-I2C | SDA / SCL / 3V3 / GND | 21 / 22 | PCF + LCD |
| J-RS485 | DI / DE_RE / RO + A/B/GND/5V | 23 / 32 / 34 | pH + temp agua |
| J-EC | EC_ADC | GPIO33 | EC analógico |
| **J-FLOW** | **YF-B5 5V / GND / signal** | **GPIO4** | **Divisor 10k serie + 20k GND → ~3.3 V** |
| J-DHT | DHT22 (opcional) | GPIO15 | Ambiente |
| — | GPIO35 | libre | EC diag / NC |

### Sensor de flujo YF-B5

```
YFB5 Rojo     → +5V
YFB5 Negro    → GND
YFB5 Amarillo ── 10k ──●── GPIO4
                       │
                    20k
                       │
                      GND
```

Calibración Hall = volumen real (balde → `flowmeter_pulses_per_liter`). EC post-dilución **no** auto-ajusta K.

Firmware: `INPUT_PULLUP` + FALLING (paridad ESP-SENSORS). Idle sin flujo: **`lvl=1`**, `F≈0`. Si `lvl=0` y pulsos fantasma → revisar cable/divisor/GND.

### Testeo serial — `FLOW_SERIAL_DEBUG=1`

```text
[FLOW] F=… valid=1 lvl=1 pulses=… Q=… total=… L rej=ok | dil=drain prog=0.12/1.00 L
```

---

## ESP32 — GPIOs (firmware)

| GPIO | Define | Rol PCB |
|------|--------|---------|
| 2 | `STATUS_LED_PIN` | LED |
| **4** | **`FLOW_SENSOR_PIN`** | **YFB5** |
| 15 | `DHT_PIN` | DHT opcional |
| 21 / 22 | I2C | PCF + LCD |
| 23 / 32 / 34 | RS485 | pH Modbus |
| 25 | `WATER_TEMP_PIN` | NC |
| 33 | `TDS_PIN` | EC ADC |
| 35 | `EC_ADC_CMP_PIN` | Libre / diag |

### Conflictos

| No cablear | Por qué |
|-----------|---------|
| DS18 en GPIO4 | Pin = YFB5; OneWire OFF permanente |
| Nivel NPN → 32/33 | RS485 / EC |
| 5 V directo al GPIO4 | Usar divisor 10k/20k → ~3.3 V |

---

## I2C

| Addr | Rol |
|------|-----|
| 0x20 | L1–L4 NPN (P3=base … P0=topo) |
| 0x24 | Dosadores (PCB); alinear `RELAY_PIN_MAPPING` si aún apunta 0x20 |
| 0x27 | LCD |

```
ESP32 Core
  GPIO2       → LED
  GPIO4       → YF-B5 (divisor 10k/20k)
  GPIO21/22   → I2C
  GPIO32/23/34 → RS485 pH (+ temp)
  GPIO33      → EC ADC
```

---

## Checklist montaje

- [ ] YF-B5 en **GPIO4** + divisor **10k/20k** (no 5 V directo)
- [ ] RS485 23/32/34; EC 33; I2C 21/22
- [ ] Sin DS18; sin niveles en 32/33
- [ ] Monitor: `[FLOW] F=… valid=… lvl=…` con `FLOW_SERIAL_DEBUG=1`

## Relacionado

- [`sensors/SENSOR_FLUJO_YFB5.md`](../sensors/SENSOR_FLUJO_YFB5.md)
- [`hydraulics/LEVEL_SENSORS_PCF8574_CAT6.md`](./hydraulics/LEVEL_SENSORS_PCF8574_CAT6.md)
