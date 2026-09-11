# ESP-NOW operativo persistente — estándar Master ↔ Slave

**Fecha:** 2026-09-10 (validado en campo)  
**Repos:** Master `ESP-HIDROWAVE-main` · Slave `ESPNOW-SLAVE-TASK-main`  
**Estado:** ✅ **HECHO Y VALIDADO** — enlace estable en bancada/producción.  
**Política:** **ASÍ DEBE PERMANECER.** No regresar a comportamientos listados abajo.

---

## 0. Congelado — no romper

Este contrato **funciona bien**. Cualquier PR/cambio que lo altere necesita justificación explícita y re-test de los criterios de la §8.

### Debe permanecer

| # | Regla | Por qué |
|---|--------|---------|
| 1 | ch**11** = solo CONFIG; **op** = relés/ACK/ping | Separar rendezvous de operación |
| 2 | Nunca persistir **11** como canal op NVS | Evita “falso sync” |
| 3 | Slave `SLAVE_JOIN_WIFI_AP=0` — sin `WiFi.begin` al router | Scan AP → canal ≠ peer → **0x3066** |
| 4 | Primera creds: ACK → hop op → rebind → suppress discovery | Link estable sin STA |
| 5 | Ya sync en op: **ACK only** (sin hop/WiFi) | Re-burst no destruye el enlace |
| 6 | Master: latch `creds-ack` → **parar** bursts en boot | Evita tormento de creds |
| 7 | Rescue Master: 1 burst tras ~90 s offline, cooldown 5 min | Recuperación rara, no spam |
| 8 | `duration==0` → ON/OFF **permanente** (sin maxDuration/safetyLock fantasma) | Recirculación continua sin reloj 59:xx |
| 9 | Soft `slaves=1` sin `RELAY_ACK` / `RULE-ACK` → **no** confiar | Online blando |

### No volver a hacer (regresiones conocidas)

- Slave juntándose al WiFi del router en loop / post-creds  
- Tratar Master visto en ch11 como sync final + guardar 11 en NVS op  
- Re-procesar WIFI_CREDENTIALS completo (hop/WiFi) con enlace ya en op  
- Master re-bursteando creds tras ACK latched en ventana boot  
- `safetyLock` / `maxDuration` 3600s inventando timer en ON continuo  

---

## 1. Roles de radio

| Dispositivo | WiFi router (AP) | ESP-NOW |
|-------------|------------------|---------|
| **Master** | Sí — STA al router; canal STA = canal **op** | Sí — relés, rules, ping |
| **Slave** | **No** (`SLAVE_JOIN_WIFI_AP=0`) | Sí — solo esto para operar |

El Slave mantiene `WiFi.mode(WIFI_STA)` **sin** `WiFi.begin` al AP: hace falta para fijar canal RF.  
Creds del Master (`SSID`/`pass`/`channel`) en el Slave sirven para **hop al canal op** (y NVS de canal); no para unirse al router.

Rollback legado Slave (solo diagnóstico): `#define SLAVE_JOIN_WIFI_AP 1` — **no** es el default de producción.

---

## 2. Contrato de canales

| Canal | Rol |
|-------|-----|
| **11** (`ESPNOW_CONFIG_CHANNEL`) | Solo rendezvous / envío de creds |
| **op** (ej. 10 = STA WiFi del Master) | Operación: relés, ACK, ping |

- Payload creds: `channel = op`.
- **Nunca** persistir **11** como canal operativo NVS.
- Tras sync: Slave lock MCD + `setDiscoverySuppressed(true)`.
- Master lost ~90s (`ESPNOW_MASTER_LOST_RETURN_CONFIG_MS`) → Slave puede volver a ch11.

---

## 3. Flujo feliz (1 vez)

```
Slave boot → ch11 (CONFIG)
Master (WiFi ok) → STA suspend → hop 11 → disc + creds (bcast+unicast)
Slave → WIFI_CREDENTIALS_ACK (aún en canal actual)
     → hop op → rebind peer Master
     → [CREDS] ESP-NOW only — sem WiFi.begin
Master → creds-ack ok → hop op → WiFi reconnect → peers ch op
Ambos en op → RELAY / RULE-ACK / ping bidireccional
```

Tiempos típicos (Master `Config.h`):

| Define | Default | Uso |
|--------|---------|-----|
| `ESPNOW_PROVISIONING_BURST_MS` | 30 s | Ventana bursts post-boot |
| Intervalo burst | ~4 s | Dentro de la ventana |
| `ESPNOW_CREDS_ACK_WAIT_MS` | 2 s | Espera ACK (+ mid-reburst) |
| `ESPNOW_LOST_SLAVE_RESCUE_AFTER_MS` | 90 s | Rescue 1 burst si offline |
| Rescue cooldown | 5 min | Evita tormento CONFIG |

---

## 4. Reglas anti-tormento (críticas)

### 4.1 Slave — creds ya sync

Si `channelSyncCompleted` y `masterChannel == creds.channel` (op válido):

- Solo **ACK** (+ save NVS canal)
- **NO** hop / **NO** `WiFi.begin`

Log: `[CREDS] já sync op=10 — ACK only (sem hop/WiFi)`

### 4.2 Slave — primera sync

- ACK → hop op → rebind → suppress discovery
- **NO** join AP (`SLAVE_JOIN_WIFI_AP=0`)
- Loop **NO** hace reconnect WiFi periódico (evita scan → ch1/2 → **0x3066**)

Log: `[CREDS] ESP-NOW only — sem WiFi.begin (SLAVE_JOIN_WIFI_AP=0)`

### 4.3 Master — latch

Si `hasWifiCredentialsAckLatched()` en ventana boot → **no más bursts**.

Rescue post-boot: 1 burst solo si Slave trusted inalcanzable ≥ 90 s; limpia latch; cooldown 5 min.

---

## 5. Relés (política duration)

| `duration` / `durationSec` | Comportamiento |
|----------------------------|----------------|
| `0` | ON/OFF **permanente** — sin timer UI / sin maxDuration fantasma |
| `> 0` | Countdown = valor exacto del comando |

Slave: `RELAY_CONFIGS` / `safetyLock` / `maxDuration` son placeholders — **no** inventan OFF ni 3600s.  
Corte real = solo timer explícito del comando (+ SafetyWatchdog si Master perdido).

---

## 6. Persistencia NVS (Slave)

| Qué | Persistir | No persistir |
|-----|-----------|--------------|
| Canal **op** (≠ 11) | Sí (MCD / creds channel) | — |
| Canal 11 | — | Como op |
| Estados ON/OFF relés | Según `Config` restore | — |
| SSID/pass | Opcional (serial `wifi_connect`) | No usar para loop AP |

---

## 7. Señales OK / FAIL

| Señal | Significado |
|-------|-------------|
| Master `[PROV] creds-ack ok` | Handshake CONFIG OK |
| Slave `[CREDS] ESP-NOW only…` | Primera sync sin AP |
| Slave `[CREDS] já sync … ACK only` | Re-burst inofensivo |
| `RELAY_ACK` / `RULE-ACK ok=1` | Enlace op estable |
| Soft `slaves=1` sin ACK de relé | Online blando — no confiar |
| `AUTH_EXPIRE` / `WiFi.begin` en Slave | Firmware viejo o `SLAVE_JOIN_WIFI_AP=1` |
| `safetyLock … 3600s` | Firmware viejo (timer fantasma) |
| `0x3066` | Peer channel ≠ home (scan/AP o hop fallido) |

---

## 8. Criterios de aceptación (validado)

Cuando el sistema está bien:

1. Hop op → `creds-ack ok` → ping / `RULE-ACK ok=1`
2. Serial Slave **sin** tormenta `AUTH_*`
3. ON recirculación `dur=0` → **sin** reloj 59:xx
4. Re-burst creds → solo `ACK only`
5. Enlace op estable tras boot (Master + Slave)

Flash de referencia: erase+upload Slave; upload Master; Slave ON → Master ON.

Debug Slave: `system_health`, `wifi_status` (AP desconectado), `mcd_cache_show`, `status`.

---

## 9. Archivos clave (no “arreglar” sin releer este doc)

| Repo | Archivos |
|------|----------|
| Slave | `Config.h` (`SLAVE_JOIN_WIFI_AP`), `ESPNowBridge.cpp` (creds), `RelayCommandBox.cpp` / `DataTypes.h` (timer), `main.cpp` (loop sin reconnect AP), `AutoCommunicationManager.h` |
| Master | `MasterSlaveManager.cpp` (`runProvisioningIfNeeded`), `EspNowChannelPolicy.cpp`, `Config.h` |

### Fuera de este contrato (conocido, no mezclar)

- `JSON_BUFFER_SIZE` / overflow al guardar rules en Master  
- pH Modbus `0xE2`, dilución unmapped, delete MQTT idempotente  

Ver también: [`HANDOFF_PROVISIONING_CICLO_11.md`](./HANDOFF_PROVISIONING_CICLO_11.md) (ciclo STA suspend histórico).
