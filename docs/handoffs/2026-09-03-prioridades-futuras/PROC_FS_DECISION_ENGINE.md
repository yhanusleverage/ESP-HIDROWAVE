# Procedimiento — FS + Decision Engine (bancada)

## Causa real del fallo

El firmware monta la partición de datos como **SPIFFS** (`platformio.ini`: `board_build.filesystem = spiffs`).  
DecisionEngine usaba **LittleFS** en la **misma** partición → log tipo `Corrupted dir pair` y:

`⚠️ DecisionEngine não iniciou — automação local desativada`

**Fix en código (3 sep 2026):** DE lee/escribe `/rules.json` vía **SPIFFS** (alineado al resto del master). No hace falta “formatear LittleFS”.

## Qué hacer en bancada

### 1) Flash del fix (obligatorio)

Desde `ESP-HIDROWAVE-main`:

```bash
pio run -e esp32dev -t upload
pio device monitor -e esp32dev
```

### 2) Serial — OK esperado

```
🧠 Inicializando Decision Engine...
✅ Decision Engine iniciado com N regras
✅ DecisionEngine local ativo
```

Si no hay `/rules.json`, verás reglas por defecto / aviso de archivo ausente — **eso es OK** (motor arrancó).

### 3) Si SPIFFS sigue roto (raro tras el fix)

Formatea la partición FS (borra UI estática + `/rules.json` en flash):

```bash
pio run -e esp32dev -t erase
pio run -e esp32dev -t upload
pio run -e esp32dev -t uploadfs
```

Luego reconfigurar WiFi si hacía falta y re-subir `data/` (HTML) con `uploadfs`.

**No hay comando serial `format` / `littlefs` en el CLI actual del master.**

### 4) Verificación rápida

| Check | Esperado |
|-------|----------|
| Sin `Corrupted dir pair` al boot | Sí |
| `DecisionEngine local ativo` | Sí |
| Comandos slave por terminal/MQTT | Siguen OK (path ACK independiente) |

## Notas

- Cloud sync de reglas está **desactivado** hoy (`checkSupabaseRules` solo log).
- `mutex_timeout` y carrera AUTO-SYNC/RETRY **no** se arreglan con format FS.
- No reactivar Global Event Bus; siguiente paso DE = correlación regla↔ACK.
