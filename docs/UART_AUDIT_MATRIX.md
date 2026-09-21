# Matriz de validación UART Master ↔ HMI

Checklist de auditoría del contrato JSON. **No es una feature del producto**: no controla bombas ni cambia firmware en runtime. Solo responde a:

> ¿Este mensaje del contrato JSON pasó de verdad entre Master y HMI, con evidencia en monitores?

| Fuente contrato | Evidencia bancada | Firmware producción |
|-----------------|-------------------|---------------------|
| [`HMI_UART.md`](../../HMI-INTERFACE-main/docs/HMI_UART.md) (HMI) | `uart-bench` / stub `uart_bench_main.cpp` (repo RX-TX) | `esp32dev` → `HmiUartBridge` |
| [`HMI_BRIDGE.md`](HMI_BRIDGE.md) (Master) | sesión 2026-09-17 | este repo |

## Resumen

| Estado | Conteo (tras correcciones) |
|--------|----------------------------|
| PASS | 6 |
| PARTIAL | 5 |
| PEND | 7 |
| N/A-bench | 8 |
| **Total** | **26** |

**Enlace confirmado (bancada 2026-09-17):** Master recibe `setpoint` + `loop_control`. HMI `Fuente:LIVE`, UART link:OK, EC/pH en espejo. ORP/DO en espejo fueron del **stub bench**, no del bridge de producción.

**Pendiente producción:** flags `ec_valid`/`ph_valid`/`temp_valid` en telemetry (Master ya emite); HMI debe consumirlos para no congelar EC vieja. Buffer JSON Master = **1536 B** + log `BUFFER LLENO`. `uart_status` / `hmi_last` disponibles en `esp32dev`.

## Qué demuestra cada estado

| Estado | Significado |
|--------|-------------|
| **PASS** | Validado con evidencia en monitores (RX/TX, espejo, `rx_bytes>0`) |
| **PARTIAL** | El mensaje llega, pero hay bug/límite pendiente |
| **PEND** | Aún no ejercitado; **no** afirma que funcione |
| **N/A-bench** | No se puede validar de verdad en stub `uart-bench` (bombas, Auto, Atlas, WiFi, ORP/DO reales) |
| **FAIL** | Probado y falló |

## Prefijos de log (grep)

| Lado | Prefijos |
|------|----------|
| Master | `[HMI UART TX]`, `[HMI UART RX]`, `[HMI UART DBG]`, `[HMI UART STATUS]`, `[HMI UART]` |
| HMI | `[UART TX]`, `[UART RX]`, `[UART DBG]`, `[UART]`, `[MIRROR]` |
| Consola Master USB | `uart_status`, `hmi_last`, `help` |

## Checks

| ID | Cat | Ítem | Dir | Wire / JSON | Master espera | HMI espera | Cómo probar | Bancada (`uart-bench`) | Producción (`esp32dev`) | Nota |
|----|-----|------|-----|-------------|---------------|------------|-------------|------------------------|-------------------------|------|
| L1 | Enlace | Cable 17/18 + GND | ambos | HMI TX17→M RX17 · M TX18→HMI RX18 · GND | `rx_bytes↑` `lines↑` | `rx_bytes↑` link=OK | `uart_status` / `[UART DBG]` | **PASS** | PEND (revalidar en prod) | Sesión 2026-09-17: Master `rx_bytes=476+` · HMI link=OK |
| L2 | Enlace | Baud / UART begin | ambos | 115200 8N1 Serial1 | `[HMI UART] RX=17 TX=18` | `[UART] master link RX=18 TX=17` | Boot logs | **PASS** | PEND | Ambos envs uart-bench |
| T1 | Telemetría | telemetry ph/ec/temp | M→H | `{"t":"telemetry","ph","ec","temp_agua"}` | `[HMI UART TX]` cada ~2s | `[UART RX] telemetry` → LIVE | Monitores + espejo | **PASS** | PEND | Bancada: pH 5.80 · EC 189. **Prod:** EC solo si `isEcValidForTelemetry()`; ver flags `*_valid` |
| T2 | Telemetría | telemetry orp/do | M→H | `orp`, `do` | campos en JSON TX | espejo ORP/DO | Espejo columnas | **N/A-bench** | **N/A-bench** | Stub bench emite ORP/DO; **`HmiUartBridge` producción NO emite** `orp`/`do`. PASS anterior era artefacto de bancada |
| T3 | Telemetría | Timeout link (sin RX) | ambos | `UART_LINK_TIMEOUT_MS` | — | UART link: sin enlace | Desconectar TX Master | **PEND** | PEND | No probado |
| S1 | Alvo / SP | setpoint EC | H→M | `{"t":"cmd","action":"setpoint","ec":N}` | RX + `cmd_ack` ok | TX + ack `ok=1` | Detalle EC → Guardar Alvo | **PARTIAL** | PEND | Master RX OK + ack true. HMI log `ok=0` = bug parse bool (fix `| false`); re-flash HMI |
| S2 | Alvo / SP | UI refleja SP / banda | USB | DataStore + bands | — | SP y Niveles en espejo | Espejo tras Guardar | **PASS** | PEND | EC SP 1510→1620 · banda 1600..1640 |
| S3 | Alvo / SP | setpoint pH | H→M | `{"action":"setpoint","ph":N}` | RX + cmd_ack | TX + ack | Detalle pH → Guardar | **PEND** | PEND | Solo se validó EC |
| C1 | Loop | loop_control completo | H→M | volumeL, homoSec, autoEc/Ph, ecLo/Hi… | RX parse OK + ack | `[UART TX] loop_control` | Boot + Guardar Alvo | **PARTIAL** | PEND | Bancada: NoMemory@512 (stub). **Prod:** buffer bridge **1536 B** + log `BUFFER LLENO` |
| C2 | Loop | Deadband ecLo/Hi phLo/Hi | H→M | campos loop_control | `tolerance=(hi-lo)/2` | banda en espejo | Guardar Alvo | **PARTIAL** | PEND | Bench solo log; prod aplica vía `applyDeadbandFromLimits` |
| C3 | Loop | autoEc / autoPh / dosingArmed | H→M | flags | HydroControl | UI Controle → Auto | Toggle Auto | **N/A-bench** | PEND | `dosingArmed` no leído en Master; autoEc/autoPh sí en prod |
| C4 | Loop | consumoDiario / consumoPh24h | H→M | bools | capa 24h | UI flags | Controle → Consumo 24h | **N/A-bench** | PEND | Solo Master producción |
| D1 | Dosificación | dose R1–R6 | H→M | `{"action":"dose","channel":"R1","ml":N}` | RX + bombas / ack | TX UI | Dosificación manual | **PEND** | PEND | Bench ack-all; no ejecuta bombas |
| D2 | Dosificación | dose_stop / dose_hold | H→M | dose_stop · dose_hold | RX + ack | TX UI | Parar / hold | **PEND** | PEND | — |
| D3 | Dosificación | phUpRelay / phDownRelay | H→M | en loop_control | mapa bombas Auto pH | UI | Asignar bomba 1–6 | **PARTIAL** | PEND | loop_control lleva `phUpRelay=4` en log |
| N1 | Nutrientes | nutrient_proportions | H→M | totalMlPerLiter, baseDose, nutrients[] | RX + updateNutrientProportions | TX Nutrientes | Menú Nutrientes | **PEND** | PEND | JSON grande — buffer Master 1536 |
| R1 | Relés | slaves_req → slaves | ambos | slaves_req / t:slaves | lista local(+Atlas) | UI Atlas | Pantalla Atlas | **N/A-bench** | PEND | Bench: ack sin inventario real |
| R2 | Relés | relay_local | H→M | relay + state + duration | actuación local | TX | UI relé master | **N/A-bench** | PEND | — |
| R3 | Relés | relay_slave | H→M | mac + relay + state | ESP-NOW Atlas | TX UI | Relé Atlas | **N/A-bench** | PEND | Requiere `ENABLE_ESPNOW` + slave |
| Y1 | Sys / WiFi | sys_info_req | ambos | → sys_info | device_id / cloud_ok | System | Pantalla System | **PEND** | PEND | Bench: `device_id=UART_BENCH` |
| Y2 | Sys / WiFi | wifi_config | H→M | ssid/password | stub / SoftAP | TX | Setup WiFi HMI | **N/A-bench** | PEND | Prod: SoftAP `ESP32_Hidropônico` |
| K1 | Calibración | calib | H→M | `{"action":"calib",…}` | ack stub v1 | TX UI | Pantalla calib | **PEND** | PEND | Stub Master+HMI |
| K2 | Calibración | pump_flow_calib | H→M | flowRate | NVS + PATCH | TX | UI flow calib | **N/A-bench** | PEND | — |
| U1 | Diagnóstico UI | Espejo Serial | USB | view \| mirror | — | tabla Param/Valor/SP | Monitor HMI | **PASS** | PEND | Espejo activo en sesión |
| U2 | Diagnóstico UI | cmd_ack visible | M→H | `{"t":"cmd_ack","ok":bool}` | ack tras cmd | `ok=1` en log | Tras setpoint/dose | **PARTIAL** | PEND | Parse bool HMI corregido — re-flash HMI |
| U3 | Diagnóstico UI | uart_status USB Master | USB | uart_status \| hmi_last | STATUS rx/tx/pines | — | Monitor Master | **PASS** (solo bench stub) | **PASS** (tras fix: `ENABLE_HMI_UART`) | Antes solo con `UART_BRINGUP`; ahora también en `esp32dev` |

## Diagnóstico EC Serial ≠ HMI

Causa raíz (producción):

1. Master omite campo `ec` si `!isEcValidForTelemetry()` (stale > `SENSOR_READING_STALE_MS` = 12 s).
2. HMI hace `isnan(ec) ? cur.ec : ec` y marca `DataSource::Live` → pantalla congela valor viejo.

Mitigación Master (este repo): telemetry siempre incluye `ec_valid`, `ph_valid`, `temp_valid`. Careo USB: `hmi_last`.

Pendiente HMI (`MasterLink.cpp`): si `ec_valid==false`, mostrar `--` / stale en vez de conservar `cur.ec`.

## Próximo paso sugerido

1. Flashear Master `esp32dev` (o `esp32dev-bringup` para DBG periódico).
2. `uart_status` → pines + ready.
3. `hmi_last` → comparar `ec` interno vs último JSON TX vs espejo HMI.
4. Guardar Alvo EC → `[HMI UART RX]` setpoint + loop_control **sin** `BUFFER LLENO`.
5. Confirmar HMI `cmd_ack … ok=1` (firmware HMI con parse bool).

## Relacionados

- [`HMI_BRIDGE.md`](HMI_BRIDGE.md) — contrato Master
- [`UART_BRINGUP.md`](UART_BRINGUP.md) — cableado / DoD físico
- [`handoffs/HANDOFF_HMI_UART_VS_SERIAL_SPAM.md`](handoffs/HANDOFF_HMI_UART_VS_SERIAL_SPAM.md) — USB spam vs UART1
