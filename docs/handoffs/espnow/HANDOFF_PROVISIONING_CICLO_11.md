# Handoff — Provisioning ciclo supremo 11 → op

**Estado:** implementado (Master + Slave)  
**Repos:** `ESP-HIDROWAVE-main` (Master) + `ESPNOW-SLAVE-TASK-main` (Slave)  
**Fecha:** 2 sep 2026

---

## Objetivo

Cuando el Master tiene credenciales WiFi pero NVS ESP-NOW vacía (post-erase), ejecutar el ciclo:

```
Master: WiFi.disconnect → ch11 → burst credenciales (payload channel=X) → ch X → WiFi.reconnect
Slave:  ch11 → recibe creds → syncRadioChannel(X) → ping → lock NVS
```

Tras el primer emparejamiento OK, ambos boot directo desde NVS (sin ch11).

---

## Constantes Master (`Config.h`)

| Define | Default | Descripción |
|--------|---------|-------------|
| `ESPNOW_CONFIG_CHANNEL` | 11 | Marco cero / sala de espera |
| `ESPNOW_PROVISIONING_BURST_MS` | 30000 | Ventana provisioning post-boot |
| `ESPNOW_PROVISIONING_STA_SUSPEND` | 1 | 1 = ciclo supremo; 0 = rollback (burst op direct) |
| `ESPNOW_PROVISIONING_STA_SUSPEND_MS` | 4000 | Budget log por burst |
| `ESPNOW_PROVISIONING_WIFI_RECONNECT_MS` | 8000 | Timeout reconnect WiFi tras burst |

**Rollback:** `#define ESPNOW_PROVISIONING_STA_SUSPEND 0` restaura comportamiento anterior (`hop config skip — burst op direct`).

---

## Logs esperados

### Master (post-erase, slave offline, ventana 30s)

```
[PROV] STA suspend start op=1 config=11
[CHANNEL] hop config=11 ok (STA down)
[PROV] burst creds+disc ch11 payload_op=1
[CHANNEL] hop op=1
[PROV] WiFi reconnect ok ch=1 (+3200ms)
[PROV] STA suspend end
[RES] radio=provisioning (STA suspend)
```

### Slave (NVS vacía, recibe creds en ch11)

```
📶 Credenciais WiFi recebidas de: EC:E3:34:...
[PROV] syncRadioChannel(1) imediato
[PROV] ping Master após sync canal
[PROV] WiFi.connect async (ESP-NOW já em ch1)
```

### Slave (timeout CONFIG sin RX previo)

```
[CHANNEL] CONFIG timeout — unlock, try op ch 1
```

Prioridad fallback: `prov_op_ch` NVS → cache MCD → `ESPNOW_CHANNEL`.

### Regresión (NVS OK, slave online)

No debe aparecer `[PROV] STA suspend` tras boot normal.

---

## Procedimiento bancada

1. Erase flash Slave; erase Master **o** borrar solo `hidro_state` + trusted slaves (conservar `hydro_system` con SSID/password).
2. Encender Master → esperar `mqtt=1`.
3. Encender Slave dentro de ventana 30s provisioning.
4. Verificar pairing en **5–15s** (ping/pong, `slaves=1`).
5. UI Automação: toggle relé Atlas → ACK sin timeout 20s.

---

## Trade-offs

| Aspecto | Impacto |
|---------|---------|
| MQTT/cloud | Blip 2–8s por burst; solo ventana 30s sin slave online |
| HTTPS | Pausado vía `isSslHotPathBusy()` durante suspend |
| WiFi reconnect fail | `HydroStateManager` recovery en loop; no restart forzado |

---

## Archivos tocados

**Master:**
- `include/Config.h`
- `include/EspNowChannelPolicy.h`
- `src/EspNowChannelPolicy.cpp`
- `src/MasterSlaveManager.cpp`
- `src/HydroSystemCore.cpp`

**Slave:**
- `src/ESPNowBridge.cpp` — handler creds no bloqueante
- `src/main.cpp` — `resolveConfigFallbackOpChannel`, NVS `hidro_slave/prov_op_ch`

---

## Relacionado

- [ESPNOW_LOCK_WINDOW.md](ESPNOW_LOCK_WINDOW.md) — ventana 5s post-handshake
- [HANDOFF_CHANNEL_SWITCH_URGENCIA.md](HANDOFF_CHANNEL_SWITCH_URGENCIA.md) — cambio canal router (futuro)
- [HANDOFF_GERAL_SESSAO_22AGO2026.md](../HANDOFF_GERAL_SESSAO_22AGO2026.md)
