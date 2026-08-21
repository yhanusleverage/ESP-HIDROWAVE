# Sensores de nivel — PCF8574 @ 0x20 (L1–L4)

Handoff hardware/software para 4 sondas NPN discretas (`level_1`..`level_4`).

**Verdad operativa (15/jul/2026):** **V2** — L1=base, L4=topo. Histórico V1 (paridad 4level): [`LEVEL_LOGIC_VERSIONS.md`](LEVEL_LOGIC_VERSIONS.md). PCF **directo**, **sin PC817**. Bancada hardware: [`4level_sensors/docs/HANDOFF_LEVELS_PCF8574.md`](../../../../4level_sensors/docs/HANDOFF_LEVELS_PCF8574.md).

---

## Topología

```
ESP32 Master (I2C SDA=21 SCL=22 ~100 kHz)
    └── PCF8574 #1 @ 0x20 — entradas P0–P7
            └── CAT6 / cable corto (GND común)
                    └── Sonda NPN × 4 (físico: P0 topo → P3 base; lógico V2: L1 base → L4 topo)
                        (NPN open-collector → pin PCF; pull-up interno del PCF8574)
```

**No** usar optoacoplador PC817 en la cadena actual. Docs antiguas que lo exigían están obsoletas.

**No** conectar GPIO 32/33 del ESP32 a niveles (conflicto RS485 DE/RE y EC ADC).

---

## Mapeo lógico (V2 activa)

Cable físico: **P0 = arriba**, **P3 = abajo**. Lógico producto: L1=base vía `LEVEL_SENSOR_PCF_PINS 3,2,1,0`.

| Lógico   | PCF8574 | Altura tanque        |
|----------|---------|----------------------|
| level_1  | P3      | Inferior — vazio     |
| level_2  | P2      | Intermedia baja      |
| level_3  | P1      | Intermedia alta      |
| level_4  | P0      | Superior — alto      |
| (reserva)| P4–P7   | Expansion (overflow / E-stop / sump) |

> **V1** (L1=P0 topo … L4=P3 base) está documentada en [`LEVEL_LOGIC_VERSIONS.md`](LEVEL_LOGIC_VERSIONS.md) para rollback.

### A0/A1/A2 (PCF8574, no variante A)

| Chip | A2 A1 A0 | Addr |
|------|----------|------|
| Niveles | 0 0 0 | **0x20** |
| Relés dosagem | 1 0 0 | **0x24** |
| LCD típico | 1 1 1 | 0x27 |

---

## Semántica NPN (active-LOW)

| Señal firmware | Significado | En PCF8574 |
|----------------|-------------|------------|
| `wet = true`   | Líquido en esa altura | NPN ON → **LOW** |
| `wet = false`  | Seco | NPN OFF → **HIGH** |

`LEVEL_NPN_ACTIVE_LOW=1` en [`Config.h`](../../../include/Config.h).

**Init crítico (RobTillaart PCF8574):** usar `pcf1.begin(0xFF)`, **nunca** `begin(false)`.  
`false` se convierte a `0` → `write8(0x00)` → pines LOW → todo MOJADO aunque no haya sonda.

---

## `water_level` agregado (V2 + fracciones)

```
!L1.wet                         → vazio        (0/4)
 L1 && !L2                      → baixo        (1/4)
 L1 && L2 && !L3 && !L4         → medio       (2/4)
 L1 && L2 && L3 && !L4          → medio_alto  (3/4)
 L4                             → alto        (4/4)
```

Ejemplo mental (1 = mojado / 0 = seco):

| L1 L2 L3 L4 | Fracción | `water_level` |
|-------------|----------|---------------|
| **0** 0 0 0 | 0/4      | **vazio**     |
| **1 0** 0 0 | 1/4      | **baixo**     |
| 1 1 **0** 0 | 2/4      | **medio**     |
| 1 1 1 **0** | 3/4      | **medio_alto** |
| 1 1 1 **1** | 4/4      | **alto**      |

`water_level_ok` depende del modo interlock (`normal` ≠ vazio / `carrera` solo alto). Ver [`LEVEL_LOGIC_VERSIONS.md`](LEVEL_LOGIC_VERSIONS.md).
Dilución / tanque alto = `level_4` wet (P0 físico).

Wet-test: **P3→GND** → L1; **P0→GND** → L4.

---

## Cableado CAT6 (sugerencia)

| Par CAT6 | Función |
|----------|---------|
| 1-2      | +V sonda (12/24 V según modelo) |
| 3-4      | GND común tanque / master |
| 5-6      | Señal NPN → PCF Px |
| 7-8      | Reserva |

Pull-ups I2C 4.7 kΩ SDA/SCL → 3.3 V en el Master.

---

## Firmware Master

| Pieza | Valor / archivo |
|-------|-----------------|
| Clase | `DiscreteLevelBank` — poll cada **200 ms** en `HydroControl::update()` (antes de Modbus/EC) |
| Flag producción | **`HIDRO_SIMULATE_WATER_LEVELS=0`** (`platformio.ini` + default `Config.h`) |
| Flag bancada sin sondas | `=1` — L1–L4 forzados ON (solo E2E) |
| Fallback GPIO 32/33 | **No usar** con RS485/EC activos |
| Poll / log | `LEVEL_POLL_MS=200`, `LEVEL_LOG_MS=1000`, `[WET-TEST]` on-change, `[LEVEL-RAW]` al boot |
| Telemetría | MQTT/HTTPS: `level_1..4`, `water_level`, `water_level_ok`, `interlock_mode`, **`levels_simulated` siempre** (`false` en producción) |
| Interlock | NVS `lvl_ilock` = `normal`\|`carrera`; MQTT `set_level_interlock` |
| Motor | `DecisionEngine` / `ScriptRunner` leen `level_1..4` / `water_level` (medio|medio_alto) |

Al boot con niveles reales **no** debe aparecer `[LEVEL] HIDRO_SIMULATE_WATER_LEVELS=1`. Debe aparecer `[LEVEL-RAW] P0=…` (H=seco esperado sin GND).

---

## Supabase / UI

- SQL: `HIDROWAVE-main/scripts/ADD_LEVEL_SENSORS_COLUMNS.sql` + `ESP-HIDROWAVE-main/scripts/ADD_LEVEL_INTERLOCK_MODE.sql`
- UI: `WaterLevelSection` / `useLevelSensors` en `/automacao` (aba Procedimentos); modo Normal/Carrera colapsable

---

## Validación bancada → producción

1. Repo `4level_sensors` (mapa V1 físico P0=L1): forzar P0→GND → L1 MOJADO en bancada sola.
2. Master V2: flash; **P3→GND** → L1; **P0→GND** → L4; ver [`LEVEL_LOGIC_VERSIONS.md`](LEVEL_LOGIC_VERSIONS.md).
3. Serial Master: `[LEVEL-RAW]`, `LEVEL` cada ~1 s, `[WET-TEST]` al cambiar; poll 200 ms.
4. **MQTT on-change:** al wet → serial `[MQTT] levels …` en &lt;1 s; bridge `PATCH device_status`; UI &lt;2 s.
5. Topic: `hidrowave/{device_id}/levels`.
6. Scripts/procedimientos: dreno → `level_1` / `vazio`; recarga → `level_4` / `alto` (V2).

### Checklist latencia UI (&lt;2 s)

| Paso | Esperado |
|------|----------|
| P0→GND (o sonda L1 wet) | Serial `[MQTT] levels …` |
| Bridge log | `PATCH device_status … water_level=` |
| `/automacao` WaterLevel / Dashboard | L1 MOJADO / agregado actualizado |

---

## Relacionado

- **Versiones L1–L4 (V1/V2):** [`LEVEL_LOGIC_VERSIONS.md`](LEVEL_LOGIC_VERSIONS.md)
- Bancada: `4level_sensors/docs/HANDOFF_LEVELS_PCF8574.md`
- Mapa GPIO: [`GPIO_PIN_MAP_CORE.md`](../GPIO_PIN_MAP_CORE.md)
- Dilución + L1–L4: [`DILUTION_FLOW_AND_LEVEL_MATRIX.md`](../../../../HIDROWAVE-main/docs/handoffs/DILUTION_FLOW_AND_LEVEL_MATRIX.md)
- MQTT topics: [`04_MODELAGEM_TOPICOS_PAYLOADS.md`](../../mqtt/04_MODELAGEM_TOPICOS_PAYLOADS.md)
