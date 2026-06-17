# Sensores de nivel sin contacto — PCF8574 + CAT6 + optoacoplador

Handoff hardware/software para 4 sondas NPN discretas (level_1..level_4).

## Topologia

```
ESP32 Master (I2C)
    └── PCF8574 #1 @ 0x20 — entradas P0-P7
            └── CAT6 (~30 cm)
                    └── Caja remota (aislamiento)
                            ├── Optoacoplador × 4
                            └── Sonda capacitiva/NPN × 4 (L1 arriba → L4 abajo)
```

## Mapeo logico

| Logico   | PCF8574 | Altura tanque        |
|----------|---------|----------------------|
| level_1  | P0      | Superior — alto      |
| level_2  | P1      | Intermedia alta      |
| level_3  | P2      | Intermedia baja      |
| level_4  | P3      | Inferior — vazio     |
| (reserva)| P4-P7   | Expansion            |

## Semantica NPN (tras opto + pull-up en PCF)

| Senal firmware | Significado              | Tipico en PCF8574 |
|----------------|--------------------------|-------------------|
| `wet = true`   | Liquido en esa altura    | NPN ON → LOW      |
| `wet = false`  | Seco en esa altura       | NPN OFF → HIGH    |

Mascara `level_invert_mask` en NVS (futuro) si el opto invierte la logica.

## `water_level` agregado

```
!L4.wet           → vazio
 L4.wet && !L3.wet → baixo
 L1.wet            → alto
 else              → medio
```

`water_level_ok` = `water_level != "vazio"` y PCF1 operativo.

## Cableado CAT6 (sugerencia)

| Par CAT6 | Funcion        |
|----------|----------------|
| 1-2      | +V sonda (12/24V segun modelo) |
| 3-4      | GND comun tanque / master      |
| 5-6      | Salida opto → PCF Px           |
| 7-8      | Reserva                        |

**No conectar GPIO del ESP32 directamente al liquido.** Usar siempre optoacoplador en la caja remota.

## Firmware

- Clase: `DiscreteLevelBank` — lee `pcf1` en `HydroControl::updateSensors()`
- Fallback: `LevelSensor` GPIO 32/33 si `!pcf1_ok`
- Telemetria MQTT/HTTPS: `level_1..4`, `water_level`, `water_level_ok`
- Motor de decision: `level_1..4`, `water_level` en `SystemState`

## Supabase

Script: `HIDROWAVE-main/scripts/ADD_LEVEL_SENSORS_COLUMNS.sql`

Columnas en `hydro_measurements` y `device_status` para UI Realtime.

## Validacion bancada

1. Serial: `LEVEL L1=MOJADO L4=SECO → water_level=vazio`
2. MQTT `hidrowave/{id}/telemetry` con 4 booleans
3. Regla `WHILE level_4 != vazio` acciona valvula
4. Auto EC bloqueado si `water_level_ok=false`

## Relacionado

- [`ARQUITETURA_SCRIPT_SEQUENCIAL_MOTOR_DECISAO.md`](../../ARQUITETURA_SCRIPT_SEQUENCIAL_MOTOR_DECISAO.md)
- Support UI: `level_1`–`level_4` en `CreateRuleModal.tsx`
