# Handoff urgente — Canal ESP-NOW (aviso no canal velho)

**Estado:** **não implementar agora.** Spec para quando for prioridade.  
**Repos:** [ESP-HIDROWAVE](../../../README.md) (Master) + `ESPNOW-SLAVE-TASK` (slave).  
**Data:** 24 ago 2026.

---

## 1. A dónde queremos llegar

Dos módulos **apartados**. Uno puede encenderse **después** del otro; la ventana en que **los dos están vivos** puede ser **~5 s**.

Por eso:

| Fase | Slave | Master |
|------|--------|--------|
| Emparejado **una vez** | Scan 1–13 **solo** si NVS vacío | Discovery / DEVICE_INFO |
| Día a día | **Ping/pong** en el canal NVS | Ping/pong |
| WiFi del Master **cambia de canal** | **No** rescan “a los montes” | Avisar en el canal **donde está el slave** |

**NVS** = último canal **de trabajo** (donde se vieron). No sustituye el aviso de mudanza.

---

## 2. Física (el comentario “no funciona”)

ESP-NOW **no emite en el 6 si el radio ya está en el 11**.

Eso **no** veta el diseño. Significa: si el Master **ya** saltó al nuevo y el slave **sigue** en el viejo, el Master **vuelve un momento** al viejo → `CHANNEL_CHANGE` → vuelve al nuevo.

MQTT/WiFi se cortan **un instante** (`esp_wifi_set_channel`, no `WiFi.reconnect()`). Eso es **aceptable**: peor es el slave en scan eterno mientras el Master está ocupado y **nunca se cruzan**.

Orden bueno si aún no saltó: **avisar en el viejo → luego ir al nuevo**.  
Hop de vuelta = solo si **ya** está en el nuevo.

---

## 3. Qué hay hoy (no está cableado)

| Pieza | Dónde | Hueco |
|-------|--------|--------|
| Tipo `TASK_MSG_CHANNEL_CHANGE = 9` | `include/ESPNowTypes.h` (Master y slave) | OK |
| `ChannelChangeNotification` | mismo header | OK |
| `ESPNowTask::sendChannelChangeNotification(old, new, reason)` | [src/ESPNowTask.cpp](../../../src/ESPNowTask.cpp) ~400 | **Nadie lo llama.** Ya hace hop a `oldChannel`, 3× broadcast, vuelve a `newChannel` |
| Detección STA `WiFi.channel()` cambió | Master loop / WiFi events | **No existe** |
| Slave RX tipo 9 + NVS + `syncRadioChannel` | `ESPNOW-SLAVE-TASK` | Struct sí; **handler no** |
| Scan 1–13 post-lock | `MultiChannelDiscovery` si `ESPNOW_FIXED_CHANNEL_ENABLED=0` | Sigue pudiendo rescanear (el problema de producto) |

---

## 4. Cómo hacerlo (cuando toque)

### Master (`ESP-HIDROWAVE`)

1. Guardar `lastEspNowChannel` (RAM + NVS).  
2. Cada N s o en evento WiFi: si `WiFi.channel()` ≠ guardado → transición.  
3. Llamar `sendChannelChangeNotification(old, nuevo, 1)`.  
4. Actualizar NVS al nuevo.  
5. No `WiFi.disconnect()` para avisar.  
6. Serial: `[CHANNEL-SWITCH] old=6 new=11 hop_back=1 mqtt_blip`.

### Slave (`ESPNOW-SLAVE-TASK`)

1. Boot: NVS → ese canal, **ping**. Scan 1–13 **solo** NVS=0 o factory.  
2. RX `TASK_MSG_CHANNEL_CHANGE` → NVS `newChannel` → `syncRadioChannel` → ping.  
3. Tras lock: **prohibido** hop 1–13 por “Master ocupado / un ping perdido”. Watchdog: reintentar **el mismo** canal; scan = último recurso (p.ej. 3 min sin PONG).  
4. ACK opcional al Master (mejor; si no, 3 TX + 50–100 ms bastan para 5 s de ventana).

### Contrato

```
oldChannel, newChannel  (1–13)
reason: 1=WiFi STA, 2=manual
```

Misma struct en los dos firmwares.

---

## 5. Cómo probar que “está bien”

1. Emparejado: slave NVS vacío → **un** scan → ping/pong.  
2. Reboot slave con Master ya up (ventana ~5 s): **sin** scan, NVS → PONG.  
3. Reboot Master con slave ya up: igual.  
4. Forzar cambio de canal AP (o `esp_wifi_set_channel` de test): slave sigue **sin** tour 1–13; Serial `[CHANNEL-SWITCH]`. MQTT reconecta solo.  
5. Slave **apagado** durante el switch: al encender, NVS viejo → un fallback scan **una vez**.

---

## 6. Tiempo (para que quede bien)

No es un parche de 2 h. El TX en Master **casi está**. Falta **quién lo dispara** + **slave que obedece** + **matar el rescan**.

| Bloque | Qué | Calendario serio |
|--------|-----|------------------|
| A | Detectar canal STA + llamar `sendChannelChangeNotification` + NVS Master | 0,5–1 día |
| B | Slave: RX, NVS, hop, ping; cortar MCD post-lock | 1–1,5 días |
| C | ACK o 3 TX + no scan por timeout corto | 0,5 día |
| D | Bancada: 3 órdenes de boot + cambio AP + MQTT blip | 1–1,5 días |
| E | Ajuste (STA hop vs AP, 5 s, watchdog) | 0,5 día |

**Total: ~4–5 días de calendario** (una persona que ya conoce los dos repos).

| Atajo | Riesgo |
|-------|--------|
| Solo llamar el send actual, slave sin RX | **0 valor** |
| MVP A+B sin D | 1–2 días, **ciego** en campo |
| Canal WiFi **fijo** en el router | Más barato si el AP no se mueve; no cubre “el router cambió solo” |

**“Bien”** = A+B+C+D, no solo el hop de `ESPNowTask.cpp`.

---

## 7. Fuera de alcance ahora

- No code en este handoff.  
- No mezclar con MQTT Lightsail / journalctl.  
- No HMI.  
- Slave **no** usa MQTT para el canal.

Cuando se implemente: abrir Agent en **los dos** firmwares; test D obligatorio antes de dar por cerrado.

---

## 8. Tiempos de scan vs espera Master

**3 s por canal está bien** (`MCD_TIMEOUT_PER_CHANNEL`). El Master a veces tarda ~1 s (peer + handshake). No bajar a 200–400 ms: el slave salta de canal antes de que el Master conteste.

| Caso | Tiempo |
|------|--------|
| Acierta NVS / canal bueno | ~1–3 s (a veces &lt; 1 s) |
| Escucha pasiva NVS | ~8 s (`MCD_PASSIVE_LISTEN_MS`) |
| Tour 1–13 (NVS vacío) | ~**40 s** (13 × 3 s + fases) |
| NVS conocido y falla | Hoy **no** recorre los 13; timeout |

**12 s** de “sala de espera” en el Master al boot (ESP-NOW vivo, sin Auto EC/dilución pesada): sí, para **reencuentro** (slave enciende después, ~5–12 s juntos). **No** cubre el primer pairing si el slave está en tour de 40 s. Ahí el Master debe **seguir visible** (ping), no “esperé 12 s y a otra cosa”. WiFi/MQTT pueden seguir; no `delay(12000)` que congele el loop.

Acelerar scan a **1 s/canal** = opcional. **3 s se queda** como default serio.

---

## 9. Ahora vs después (canal)

**Hoy:** slave busca (scan si hace falta); si pierde ping puede rescanear; cambio de AP sin aviso; `sendChannelChangeNotification` **no se llama**; ventana corta 5 s se pierde en el tour.

**Después:** un discover; NVS + ping/pong; Master avisa mudanza (hop breve al canal viejo si hace falta); MQTT parpadea; scan 1–13 solo NVS vacío o último recurso (minutos).

