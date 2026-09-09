# ESP-NOW provisioning — handshake estável (Master ↔ Slave)

**Fecha:** 2026-09-09  
**Repos:** Master `ESP-HIDROWAVE-main` · Slave `ESPNOW-SLAVE-TASK-main`

## Contrato de canales

| Canal | Rol |
|-------|-----|
| **11** (`ESPNOW_CONFIG_CHANNEL`) | Solo rendezvous / envío de creds |
| **op** (ej. 10 = STA WiFi del Master) | Operación: relés, ACK, ping |

Creds payload lleva `channel = op`. Nunca persistir **11** como canal operativo NVS.

## Flujo feliz (1 vez)

```
Slave boot → ch11
Master (WiFi ok) → STA suspend → hop 11 → disc + creds (bcast+unicast)
Slave → WIFI_CREDENTIALS_ACK (aún en canal actual) → hop op → rebind peer
Master → creds-ack ok → hop op → WiFi reconnect → peers ch op
Ambos en op → RELAY / RULE-ACK bidireccional
```

Tiempos típicos (Master `Config.h`):

- Ventana boot bursts: `ESPNOW_PROVISIONING_BURST_MS` = 30 s (cada ~4 s)
- Espera ACK: `ESPNOW_CREDS_ACK_WAIT_MS` = 2 s (+ mid-reburst)
- Rescue offline: tras `ESPNOW_LOST_SLAVE_RESCUE_AFTER_MS` = 90 s, cooldown 5 min

## Regla anti-tormento (crítica)

### Slave (`ESPNowBridge` · `WIFI_CREDENTIALS`)

Si **ya** `channelSyncCompleted` y `masterChannel == creds.channel` (op válido):

- Enviar **solo ACK** (+ save NVS opcional)
- **NO** `WiFi.disconnect` / `WiFi.begin` / hop de nuevo  

Re-procesar creds completas con el enlace ya en op provoca scan WiFi → canal ≠ peer → **0x3066** y Master ve offline.

Log esperado: `[CREDS] já sync op=10 — ACK only (sem hop/WiFi)`

### Master (`runProvisioningIfNeeded`)

Si `hasWifiCredentialsAckLatched()` dentro de la ventana de boot → **no más bursts**.

Rescue post-boot: 1 burst solo si Slave trusted inalcanzable ≥ 90 s; limpia latch; cooldown 5 min.

## Señales de OK / FAIL

| Señal | Significado |
|-------|-------------|
| Master `[PROV] creds-ack ok` | Handshake CONFIG OK |
| Slave `[CREDS] já sync … ACK only` | Re-burst inofensivo |
| `RELAY_ACK` / `RULE-ACK ok=1` | Enlace op estable |
| `creds-ack timeout` + muchos `CREDENCIAIS` + `0x3066` | Re-negociación destruyendo link (bug viejo) |
| Soft `slaves=1` sin ACK de relé | Online blando — no confiar |

## Flash / prueba

1. Erase+upload **Slave** (recomendado); upload **Master**
2. Slave ON → Master ON
3. Serial: creds → hop op → `creds-ack ok` → ON con `RULE-ACK ok=1`
4. Si Master reenvía creds: Slave debe loguear **ACK only**, sin fallar WiFi/0x3066

## Archivos clave

- Slave: `ESPNowBridge.cpp` (`WIFI_CREDENTIALS`), `main.cpp` (return CONFIG si Master lost)
- Master: `MasterSlaveManager.cpp` (`runProvisioningIfNeeded`), `EspNowChannelPolicy.cpp` (burst)
- Constantes: `Config.h` (ambos repos)
