# HMI UART Bridge — Master ESP-HIDROWAVE

Bridge JSON línea a línea entre el **Master** (ESP32 clásico) y la pantalla **JC3248W535** (ESP32-S3 + LVGL).

Contrato completo del display: [`HMI-INTERFACE-main/docs/HMI_UART.md`](../../HMI-INTERFACE-main/docs/HMI_UART.md).

## Cableado

| Señal | Master ESP32 | HMI ESP32-S3 |
|-------|--------------|--------------|
| TX    | GPIO **18**  | RX GPIO 18   |
| RX    | GPIO **17**  | TX GPIO 17   |
| GND   | GND          | GND          |

Baud **115200**, 8N1. UART del Master: **Serial1** (`HmiUartBridge.cpp`).

## Build

`ENABLE_HMI_UART=1` en `platformio.ini` / `Config.h`. Pines en:

- `HMI_UART_RX_PIN` = 17  
- `HMI_UART_TX_PIN` = 18  
- `HMI_TELEMETRY_INTERVAL_MS` = 2000  

## Master → HMI

| Tipo | Cuándo |
|------|--------|
| `telemetry` | Cada ~2 s (ph, ec, temp_agua) |
| `cmd_ack` | Tras cada comando de proceso |
| `sys_info` | Respuesta a `sys_info_req` |
| `slaves` | Respuesta a `slaves_req` |
| `wifi_config_ack` | Stub v1 (ok=false; usar SoftAP Master) |

## HMI → Master (implementado v1)

| `action` | Handler |
|----------|---------|
| `dose` / `dose_stop` / `dose_hold` | `RelayCoordinator` + flowRate calibrado |
| `nutrient_proportions` | `updateNutrientProportions` + `applyRecipeGain` (+ `flowRate` si HMI calibró) |
| `pump_flow_calib` | Router: `nutrients[].flowRate` o `ph flow_rate_ph_*` → NVS + PATCH Supabase |
| `loop_control` | Auto EC/pH, deadband Alvo, consumo 24h, volumen, pulsos |
| `setpoint` | EC / pH setpoint |
| `relay_local` / `relay_slave` | Actuación local / ESP-NOW |
| `calib` | Ack stub |
| `wifi_config` | Stub (SoftAP `ESP32_Hidropônico` / `hidrosetup`) |
| `sys_info_req` / `slaves_req` | Respuesta inmediata |

## Deadband

Desde `ecLo`/`ecHi` y `phLo`/`phHi` en `loop_control`:

```
tolerance = (hi - lo) / 2   (mínimo 1 µS EC / 0.01 pH)
```

No se usa deadband fijo de 50 µS.

## Prueba en bancada

1. Flashear **envs bring-up**: Master `esp32dev-bringup`, HMI `esp32-s3-hmi-bringup` (`DATA_SOURCE_SIM=0` en prod HMI).
2. Monitor Master (COM5) → `[HMI UART] RX=17 TX=18`; cada ~2 s `[HMI UART TX] telemetry`.
3. Monitor HMI (COM4) → al boot `[UART TX] loop_control`; tras cable OK → `[UART RX] telemetry ec=…`.
4. En **cada** monitor escribir `uart_status` → debe listar pines y `rx_bytes` acumulados.
5. DoD: `rx_bytes > 0` bidireccional; Master recibe `loop_control`; HMI recibe EC real.

Guía extendida: [`UART_BRINGUP.md`](UART_BRINGUP.md).

## Troubleshooting

| Síntoma | Acción |
|---------|--------|
| `rx_bytes=0` ambos lados | Ver cable cruzado 17↔17, 18↔18, GND común — [`UART_BRINGUP.md`](UART_BRINGUP.md) |
| Master TX OK, HMI RX 0 | Revisar Master TX GPIO18 → HMI RX GPIO18 |
| HMI TX OK, Master RX 0 | Revisar HMI TX GPIO17 → Master RX GPIO17 |
| Espejo muestra LIVE + EC 470 sin UART | Usar env `-bringup`; fuente SIM hasta primer telemetry |
| Monitor Master inundado con `Tentando reconectar WiFi` | Bug reconnect corregido; usar `wifi_status` en bring-up para ver fase |

## Archivos

- `include/HmiUartBridge.h`
- `src/HmiUartBridge.cpp`
- Integración: `HydroSystemCore::begin()` / `loop()`
