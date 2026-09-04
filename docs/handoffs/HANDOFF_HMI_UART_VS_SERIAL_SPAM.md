# HANDOFF — HMI UART vs spam Serial / `mutex_timeout`

**Fecha:** 3 sep 2026  
**Contexto:** bancada Master con HMI JC3248W535 + ESP-NOW Atlas + MQTT. Logs USB con `[Cache] JSON…`, `[HMI UART TX]`, `[SLAVE-LINK] mutex_timeout`.

## Pregunta

¿El spam de Serial (USB) y `mutex_timeout` pueden **atrapar** la comunicación HMI?

**Respuesta:** sí, de forma **indirecta** (CPU + `loop()` atrasado). **No** porque HMI y USB compartan el mismo cable.

---

## Dos UARTs distintas

| Enlace | Puerto | Pines | Quién habla |
|--------|--------|-------|-------------|
| **HMI** | `UART1` (`HardwareSerial(1)` en `HmiUartBridge.cpp`) | RX **17** / TX **18** | Pantalla JC3248W535 |
| **PC / PlatformIO Monitor** | `Serial` (UART0 / USB) | COM USB | Logs de debug |

El JSON `[Cache] Primeiros 200 chars…` **no** sale por GPIO 17/18. Sale por USB.

Cable HMI (sin cambio):

```
HMI TX(17) → Master RX(17)
Master TX(18) → HMI RX(18)
GND común
Baud 115200 8N1
```

Guía de cable / bring-up: [`docs/HMI_BRIDGE.md`](../HMI_BRIDGE.md), [`docs/UART_BRINGUP.md`](../UART_BRINGUP.md).

---

## Dónde sí se pisan: el mismo `loop()`

En `HydroSystemCore::loop()` (Core Arduino), en cada iteración:

1. `hmiUartBridge.loop()` — **poll** de RX HMI (no hay task dedicada)
2. `hmiUartBridge.maybePublishTelemetry()` — TX HMI cada `HMI_TELEMETRY_INTERVAL_MS` (2000)
3. MQTT, ESP-NOW, Cache JSON, `Serial.printf` de ping/pH/`[RES]`…

`Serial.print` **bloquea** si el buffer USB está lleno (mucho texto a 115200). Mientras bloquea, **no** se vuelve a llamar `HmiUartBridge::loop()`.

La HMI se lee a mano:

```cpp
// src/HmiUartBridge.cpp — loop()
while (HmiSerial.available() > 0) {
    const char c = static_cast<char>(HmiSerial.read());
    // … línea JSON …
}
```

FIFO hardware UART1 en ESP32 ≈ **128 bytes**. Si el `loop()` se atrasa cientos de ms, overflow → comandos HMI perdidos o JSON cortado.

### Eco USB de cada paquete HMI (importante)

`emitJson()` escribe **dos veces** el mismo JSON:

```cpp
void HmiUartBridge::emitJson(const JsonDocument& doc) {
    serializeJson(doc, HmiSerial);   // UART1 → pantalla
    HmiSerial.print('\n');
    Serial.print("[HMI UART TX] ");  // UART0 → PC
    serializeJson(doc, Serial);
    Serial.println();
}
```

Telemetría cada 2 s hacia la HMI **más** el mismo payload al monitor. Con spam de Cache encima, el Core pasa rato imprimiendo.

---

## `mutex_timeout` no es el UART de la HMI

Log típico:

```
[SLAVE-LINK] event=mutex_timeout mac=14:33:5C:38:BF:60 queue=0 heap=…
```

Es el mutex **`trustedSlavesMutex`** en `MasterSlaveManager` (lista ESP-NOW), timeout de espera **1 s**. **No** cierra UART1 ni GPIO 18.

Relación **indirecta**:

```
Spam Serial / Cache serializa slaves
        ↓
loop() lento, Serial.print espera
        ↓
otra task (espNowTask) o el mismo loop espera el mutex
        ↓
mutex_timeout
```

Síntoma de bancada visto junto: Cache JSON + ping/pong + `[HMI UART TX]` + `mutex_timeout`. Atlas lookup puede fallar un ciclo; HMI puede ir a saltos. **No** significa “el mutex tapó el pin 18”.

Código: `MasterSlaveManager::getTrustedSlave()` / `lookupTrustedSlave()` → `logSlaveLink("mutex_timeout", …)`.

---

## Qué notarías (HMI vs cable)

| Síntoma | Causa más probable |
|---------|-------------------|
| `rx_bytes=0` ambos lados | Cable / GND — ver UART_BRINGUP |
| HMI botones a veces no responden, Core sigue imprimiendo USB | Loop lento / FIFO RX overflow |
| Telemetría HMI a saltos (no ~2 s) | `loop()` ocupado en Serial/Cache |
| JSON inválido / línea truncada HMI | Overflow o `lineBuf_` 768 B |
| `mutex_timeout` + Cache spam | Contención lista slaves + UART0 |

---

## Flags / archivos

| Ítem | Dónde |
|------|--------|
| `ENABLE_HMI_UART` | `Config.h` / `platformio.ini` |
| Pines / baud / 2 s telemetry | `HMI_UART_RX_PIN`, `HMI_UART_TX_PIN`, `HMI_UART_BAUD`, `HMI_TELEMETRY_INTERVAL_MS` |
| Debug HMI en USB | `UART_LINK_DEBUG` (default 0 salvo `UART_BRINGUP`) |
| Eco `[HMI UART TX]` | **siempre on** hoy en `emitJson` (independiente de `UART_LINK_DEBUG`) |
| Cache slaves JSON en Serial | `HydroSystemCore.cpp` (~cada pocos s aunque `webServerTask=nullptr`) |
| `ESPNOW_LOCK_DEBUG` | `Config.h` (1 = más logs lock) |
| `RESOURCE_SERIAL_DEBUG` | `[RES]` periódico |

---

## Optimización recomendada (cuando se implemente)

Orden de ROI (no mezclar con HTTPS; no flash slave por esto):

1. **Quitar eco USB en producción** — `emitJson` solo `HmiSerial`; `[HMI UART TX]` detrás de `UART_LINK_DEBUG` o flag propio.
2. **Silenciar Cache JSON** en Serial si admin HTTP está off (`webServerTask=nullptr`).
3. `ESPNOW_LOCK_DEBUG=0` en prod.
4. `RESOURCE_SERIAL_DEBUG=0` o intervalo más largo.
5. No hace falta bajar telemetría HMI de 2 s si se elimina el duplicado USB.

Validar después: `loop/s` en `[RES]`, menos `mutex_timeout`, HMI `rx_bytes` estable, clic HMI responde.

---

## Qué NO es este problema

- MQTT / `rule_executed` / bridge Lightsail
- Firmware Atlas (ESP-NOW slave)
- Cable HMI “lleno” de logs USB

---

## Relacionado

- [`docs/HMI_BRIDGE.md`](../HMI_BRIDGE.md) — contrato y handlers
- [`docs/UART_BRINGUP.md`](../UART_BRINGUP.md) — bring-up pines
- [`docs/handoffs/firmware/ESP32_MASTER_RESOURCE_MAP.md`](firmware/ESP32_MASTER_RESOURCE_MAP.md) — CPU / heap / `[RES]`
