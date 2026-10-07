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

| Tipo | Cuándo | Campos |
|------|--------|--------|
| `telemetry` | Cada ~2 s | `ph`, `ec`, `temp_agua` (solo si lectura válida/fresca); **siempre** `ph_valid`, `ec_valid`, `temp_valid` (bool) |
| `cmd_ack` | Tras cada comando de proceso | `action`, `ok`, `commandId` |
| `sys_info` | Respuesta a `sys_info_req` | `device_id`, `cloud_ok`, `process_bridge`, snapshot provisión (`has_wifi`, `wifi_connected`, `ssid`, `password`, `email`, `device_name`, `location`) |
| `slaves` | Respuesta a `slaves_req` | array `slaves[]` |
| `wifi_config_ack` | Tras `wifi_config` | `ok` + `device_id` |

**Nota EC congelada en HMI:** si `ec_valid=false`, el Master **omite** el campo `ec`. La HMI debe tratar `ec_valid==false` como dato no usable (mostrar `--` / stale) y **no** conservar el valor anterior marcándolo LIVE. Implementación HMI: `MasterLink` + `DataStore` (2026-09-20). Auditoría: [`UART_AUDIT_MATRIX.md`](UART_AUDIT_MATRIX.md) · paridad MVP: [`handoffs/HANDOFF_MVP_WEB_HMI_PARITY.md`](handoffs/HANDOFF_MVP_WEB_HMI_PARITY.md).

**WiFi bidireccional:** SoftAP escribe NVS `hydro_system`. HMI pide `sys_info` y precarga wizard. HMI → `wifi_config` escribe el **mismo** NVS + `wifi_config_ack` + **reboot ~2.5 s** (paridad SoftAP: sale de WIFI_CONFIG_MODE y conecta STA). SoftAP `ESP32_Hidropônico` / `hidrosetup` = vía A.

**Atlas rule-lock:** respuesta `slaves` puede incluir por relé `locked` / `lock_reason` / `lock_label`. Manual `relay_slave` sobre relé lastreado → `cmd_ack ok=false`.

Buffer JSON RX Master: **1536 B** (`kJsonCapacity`). Si un `loop_control` / `nutrient_proportions` no cabe → log `[HMI UART] BUFFER LLENO`.

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
| `wifi_config` | Mismo NVS SoftAP (`hydro_system`) + `wifi_config_ack` + reboot ~2.5 s |
| `master_reboot` | `cmd_ack` + `ESP.restart()` (~400 ms) — no borra NVS |
| `factory_reset` | Limpia `hydro_system` + `wifi_creds` y borra `/rules.json`, `cmd_ack`, reinicio (~500 ms) — no formatea la flash |
| `sys_info_req` / `slaves_req` | Respuesta inmediata |

## Deadband

Desde `ecLo`/`ecHi` y `phLo`/`phHi` en `loop_control`:

```
tolerance = (hi - lo) / 2   (mínimo 1 µS EC / 0.01 pH)
```

No se usa deadband fijo de 50 µS.

## Prueba en bancada

1. Flashear Master `esp32dev` o `esp32dev-bringup`; HMI `esp32-s3-hmi-bringup` / `uart-bench` (`DATA_SOURCE_SIM=0`).
2. Monitor Master → `[HMI UART] RX=17 TX=18`; cada ~2 s `[HMI UART TX] telemetry` (con `ec_valid`/`ph_valid`/`temp_valid`).
3. Monitor HMI → al boot `[UART TX] loop_control`; tras cable OK → `[UART RX] telemetry ec=…`.
4. En Master USB: `uart_status` (pines/ready) y `hmi_last` (último JSON TX + EC interna / age_ms).
5. DoD: enlace bidireccional; Master recibe `loop_control` sin `BUFFER LLENO`; HMI recibe EC real cuando `ec_valid=true`.

Checklist completo: [`UART_AUDIT_MATRIX.md`](UART_AUDIT_MATRIX.md).  
Guía física: [`UART_BRINGUP.md`](UART_BRINGUP.md).

## Troubleshooting

| Síntoma | Acción |
|---------|--------|
| `rx_bytes=0` ambos lados | Ver cable cruzado 17↔17, 18↔18, GND común — [`UART_BRINGUP.md`](UART_BRINGUP.md) |
| Master TX OK, HMI RX 0 | Revisar Master TX GPIO18 → HMI RX GPIO18 |
| HMI TX OK, Master RX 0 | Revisar HMI TX GPIO17 → Master RX GPIO17 |
| Espejo muestra LIVE + EC 470 sin UART | Usar env `-bringup`; fuente SIM hasta primer telemetry |
| Monitor Master inundado con `Tentando reconectar WiFi` | Bug reconnect corregido; usar `wifi_status` en bring-up para ver fase |
| HMI a saltos / botones sin ACK + USB lleno de `[Cache]` / `[HMI UART TX]` / `mutex_timeout` | **No es el mismo UART.** USB `Serial` bloquea el `loop()` y puede overflow del FIFO HMI. Ver [`handoffs/HANDOFF_HMI_UART_VS_SERIAL_SPAM.md`](handoffs/HANDOFF_HMI_UART_VS_SERIAL_SPAM.md) |

## Archivos

- `include/HmiUartBridge.h` / `src/HmiUartBridge.cpp`
- Integración: `HydroSystemCore::begin()` / `loop()`
- Auditoría: [`UART_AUDIT_MATRIX.md`](UART_AUDIT_MATRIX.md)
- Consola USB: `uart_status`, `hmi_last` (requiere `ENABLE_HMI_UART=1`)
