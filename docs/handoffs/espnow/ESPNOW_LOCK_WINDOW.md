# ESP-NOW: ventana de lock (Fase 1)

**Fecha:** 22 ago 2026  
**Master:** `ESP-HIDROWAVE-main` · **Slave:** `ESPNOW-SLAVE-TASK-main`

## Problema

El Slave escucha ~8 s en el canal del NVS (bancada: 5), luego salta a 1/6/11. El Master lo ve y a la vez lanza HTTPS (`sslBusy`). El handshake no cierra. `Operacional: Sim` no es `Online`.

## Patrón (Espressif / radio único)

1. Scan **una vez** si NVS vacío.
2. Si NVS tiene canal: **quedarse** (el STA del Master no se mueve).
3. Al encuentro: Master pausa la nube **5 s** (no se para el riego).
4. Consistencia: `lastRxAgeMs` local (no comparar `millis()` entre placas). `seq` en payload = fase posterior.

## Los 5 relojes

| # | Qué | Dónde | Fase 1 |
|---|-----|--------|--------|
| 1 | Ventana radio Master | `addTrustedSlave` | **5 s**, sin GET EC / poll comandos / telemetry MQTT |
| 2 | Quedarse en NVS | `discoverMaster` | NVS 2–13: **no** Fase 1 (1/6/11) |
| 3 | Heartbeat 15 s | `SafetyWatchdog` | Sin cambio; sirve **después** del lock |
| 4 | HTTPS/MQTT | `HydroSystemCore::update` | Pausados si ventana activa |
| 5 | Re-scan | `performRediscoveryIfNeeded` | No hop si ya en canal NVS; 180 s si NVS vacío |

## Validar

Slave: arranque en 5, sin `Canal 1/6/11` si NVS=5. Logs `[LOCK]` / `[SCAN]`.  
Master: `[LOCK] window start 5s` y en esos 5 s no hay `[SYNC] EC config poll` grande.  
Lista: `Online: Sim` tras handshake.

## Apagar debug Master (después)

`ESPNOW_LOCK_DEBUG 0` en `ESP-HIDROWAVE-main/include/Config.h` cuando el enlace esté validado. El Slave puede dejar logs `[LOCK]` un tiempo.
