# Histórico de versiones — mapeo lógico L1–L4

Documento para **no perder** la semántica validada. El cable físico PCF **no cambia**: **P0 = arriba (topo)**, **P3 = abajo (base)**. Solo cambia qué índice lógico (`level_1`…`level_4`) apunta a cada pin.

---

## V1 — Paridad `4level_sensors` (validada Jul 2026)

**Estado:** archivada / referencia. Activa en bancada [`4level_sensors`](../../../../4level_sensors/docs/HANDOFF_LEVELS_PCF8574.md).

| Lógico   | PCF | Altura        |
|----------|-----|---------------|
| level_1  | P0  | Topo — alto   |
| level_2  | P1  | Intermedia alta |
| level_3  | P2  | Intermedia baja |
| level_4  | P3  | Base — vazio  |

```
LEVEL_SENSOR_PCF_PINS 0, 1, 2, 3
```

**Agregado `water_level`:**

```
!L4.wet            → vazio
 L4.wet && !L3.wet → baixo
 L1.wet            → alto
 else              → medio
```

- Dilución / tanque alto = **L1** wet  
- Wet-test bancada: **P0→GND** → L1 MOJADO  
- UI: Nível 1 = topo, Nível 4 = base  

---

## V2 — L1 base / L4 topo (producto HydroWave, Jul 2026)

**Estado:** **activa en Master + UI** (esta versión).

Mismo cable: P0 arriba, P3 abajo. Remapeo lógico:

| Lógico   | PCF | Altura        |
|----------|-----|---------------|
| level_1  | P3  | Base — vazio  |
| level_2  | P2  | Intermedia baja |
| level_3  | P1  | Intermedia alta |
| level_4  | P0  | Topo — alto   |

```
LEVEL_SENSOR_PCF_PINS 3, 2, 1, 0
```

**Agregado `water_level` (fracciones 0/4–4/4):**

```
!L1.wet                         → vazio        (0/4)
 L1 && !L2                      → baixo        (1/4)
 L1 && L2 && !L3 && !L4         → medio       (2/4)
 L1 && L2 && L3 && !L4          → medio_alto  (3/4)
 L4                             → alto        (4/4)
```

- Dilución / tanque alto = **L4** wet  
- Wet-test: **P3→GND** → L1 MOJADO (base); **P0→GND** → L4 MOJADO (topo)  
- UI: Nível 1 = base, Nível 4 = topo  

### Interlock Auto EC/pH (`water_level_ok`)

| Modo NVS `lvl_ilock` | Libera dosagem cuando |
|----------------------|------------------------|
| `normal` (default)   | `water_level != vazio` |
| `carrera`            | `water_level == alto` (4/4) |

MQTT: `action=set_level_interlock`, `mode=normal|carrera`. Telemetry/levels publican `interlock_mode`.

Compat bridge: `medio_baixo` → `medio` (2/4).

### Ejemplo de regla — drenar hasta 3/4 (medio_alto)

- Opción A: `WHILE water_level != medio_alto` + válvula dreno ON (salir al llegar a 3/4).
- Opción B (bits): `WHILE level_4 == seco` y condicionar con L3 según el editor.

Carrera: UI modo Carrera → Auto EC/pH solo liberado si `alto`.

---

## Cómo volver a V1

1. [`Config.h`](../../../include/Config.h): `#define LEVEL_SENSOR_PCF_PINS 0, 1, 2, 3`  
2. [`DiscreteLevelBank.cpp`](../../../src/DiscreteLevelBank.cpp): restaurar `deriveWaterLevel` de V1 (bloque arriba)  
3. [`HydroControl.cpp`](../../../src/HydroControl.cpp): `isTankHighCapacitive` → `isWet(1)`  
4. UI [`water-level-display.ts`](../../../../HIDROWAVE-main/src/lib/water-level-display.ts): L1=topo, L4=base  
5. Flash + checklist wet  

---

## Notas que no cambian entre V1 y V2

- NPN active-LOW: LOW = MOJADO, HIGH = SECO  
- `pcf1.begin(0xFF)` — nunca `begin(false)`  
- Poll 200 ms, `[WET-TEST]`, MQTT `/levels` on-change  
- `levels_simulated=false` en producción  
