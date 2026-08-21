# GlobalEventBus — APAGADO del build

**12/jul/2026**

`GlobalEventBus.cpp` y `DecisionEngineLoop.cpp` quedan **fuera del firmware** (`platformio.ini` `build_src_filter`).

- No hay WebSocket EventBus hacia la UI.
- El camino vivo de reglas es **DecisionEngine + ScriptRunner + DecisionEngineIntegration** (callbacks / ESP-NOW / MQTT), no EventBus.

Archivos `.h` pueden quedar en el árbol; no se compilan los `.cpp`.
