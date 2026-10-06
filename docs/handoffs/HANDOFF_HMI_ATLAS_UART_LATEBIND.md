# Handoff — HMI Atlas offline con slave ESP-NOW vivo

**Fecha:** 2026-09-23  
**Repos:** ESP-HIDROWAVE-main (Master) + HMI-INTERFACE-main  
**No usar:** `RX-TX/` (referencia)

## Síntoma

El Master ve el Atlas por radio (`slaves=1`, ping/pong, `Slaves online: 1/2`), pero la HMI muestra los relés Atlas offline. `slaves_req` respondía solo `mac:local`.

```text
[HMI UART] slaves_req SIN gestor — late-bind no aplicado
[HMI UART] slaves_req atlas=0 trusted=-1 mutex=0
[TASK] Slaves online: 1 / 2
```

## Sendero de boot

1. `stateManager.begin()` conecta WiFi y llama `switchToHydroActive()` **antes** de crear `MasterSlaveManager`.
2. `HydroSystemCore::begin()` hace `HmiUartBridge::attach` con `masterManager == nullptr`.
3. Más tarde `main` crea el gestor y llama `HydroSystemCore::setMasterManager()` (late bind). Eso actualiza relés, reglas y ESP-NOW.
4. El late bind **no** actualizaba el contexto UART. `sendSlavesList()` seguía sin gestor y emitía solo el Master local.
5. La HMI, sin MAC Atlas, deja el hub offline. El ON/OFF no sale como `relay_slave`.

## Núcleo

| Capa | Verdad | Quién la usa |
|------|--------|----------------|
| ESP-NOW / `MasterSlaveManager` | Slave online, MAC, estados | Tarea ESP-NOW, ping, comandos |
| UART `t:slaves` | Copia que ve la HMI | Hub Relés, ON/OFF Atlas |
| `relay_local` | Bombas PCF del Master | Sección Master (bancada sin PCF = NACK) |
| `relay_slave` + MAC | Relé Atlas | Solo si el inventario UART trae esa MAC |

`slaves=1` en `[RES]` **no** es el inventario UART. La HMI solo cree lo que llega en `t:slaves`.

## Fix

`HmiUartBridge::bindMasterManager()` asigna solo el puntero MSM (no reabre UART ni pisa hydro/coordinator). `HydroSystemCore::setMasterManager()` lo llama al final del late bind. Log: `[HMI UART] UART bridge MSM late-bind`.

## Debug de cadena

- Master: `slaves_req atlas=N trusted=M mutex=0|1`. `SIN gestor` = late-bind no aplicado.
- HMI: `[UART] inventario local=1 atlas=N`. `atlas=0` = hub offline esperado. Copia en pantalla Sistema (`dumpSystemSerial`).

## DoD

Flashear Master + HMI. Actualizar Relés:

- `atlas>=1` y MAC del Atlas con `online:true`
- ON del relé Atlas → `action":"relay_slave"` (no `relay_local`)
- Serial HMI: `inventario … atlas=1`

## Ampliación

- Varios Atlas: el snapshot admite hasta 6. La HMI usa el primero (`firstEspNowIndex`). Un selector de MAC es UI, no otro protocolo.
- Estados `on`: van en `t:slaves` al pedir inventario; no hay push por relé.
- Web: estado cloud por MQTT. `mqtt=0` no se arregla con este late-bind.
- Bombas: `relay_local` + PCF. NACK en ESP32 de test = hardware ausente.
