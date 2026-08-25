# Handoff — validación ACL flota (`%c`)

**Fecha:** 22 ago 2026  
**Broker:** Lightsail `15.175.109.90:1883`  
**Core de bancada:** `ESP32_HIDRO_1A575C` (client id = ese id)  
**Relacionado:** [ALIGN_BROKER_MASTER.md](ALIGN_BROKER_MASTER.md) · [acl.example](../../infra/mqtt/mosquitto/acl.example)

## Objetivo

- Placa Core **nueva** se conecta **sin** `mosquitto_passwd` por MAC.
- Cada Core solo lee/escribe `hidrowave/<su_device_id>/#`.
- El **email** del cliente no va en MQTT: dueño = fila Supabase + RLS.
- Atlas Online en la web = `relay_slaves.last_update` &lt; 90 s (bridge MQTT), no el Serial.

`%c` = client id de **esta** conexión. El Core ya se presenta como `ESP32_HIDRO_XXXXXX`.

## ACL objetivo (un user para todos los ESP)

`passwd`: solo tres users — `bridge_internal`, `hidrowave`, `mqtt_esp`.

```
user bridge_internal
topic read hidrowave/+/heartbeat
topic read hidrowave/+/telemetry
topic read hidrowave/+/levels
topic read hidrowave/+/status
topic read hidrowave/+/ec_operation
topic read hidrowave/+/dose
topic read hidrowave/+/ph_operation
topic read hidrowave/+/ph_dose
topic read hidrowave/+/ec_metric
topic read hidrowave/+/ph_metric
topic read hidrowave/+/ec_dilution
topic read hidrowave/+/command_ack
topic read hidrowave/+/relay/state

user hidrowave
topic write hidrowave/+/command
topic read hidrowave/+/#

user mqtt_esp
topic read hidrowave/%c/command
topic write hidrowave/%c/#
```

Traducción en vivo: Core `ESP32_HIDRO_1A575C` → solo `hidrowave/ESP32_HIDRO_1A575C/#`.

## Firmware (hoy vs paso 2)

| Hoy | Para esta ACL |
|-----|----------------|
| User MQTT = `mqtt_` + device_id | User = **`mqtt_esp`** (misma seña flota en `secrets.ini`) |
| Client id = `ESP32_HIDRO_XXXXXX` | Igual (eso es `%c`) |

**Paso 2 (no este handoff):** cambiar `MqttClient.cpp` para login `mqtt_esp`. Sin eso, Mosquitto seguirá pidiendo `mqtt_ESP32_HIDRO_1A575C` en `passwd`.

## Pre-requisito red

Si `Test-NetConnection 15.175.109.90 -Port 22` / `1883` = False: firewall Lightsail o instancia parada. **No es el ACL.** Abrir 22 y 1883, luego validar.

## Cómo validar (cuando 1883/22 estén abiertos)

SSH: `ubuntu@15.175.109.90`

```bash
sudo systemctl is-active mosquitto hidrowave-bridge
sudo journalctl -u hidrowave-bridge -f
```

### Tests

1. **Core A** (`ESP32_HIDRO_1A575C`) publica en `hidrowave/ESP32_HIDRO_1A575C/relay/state`. OK.
2. `mosquitto_sub` con **otro** client id (`ESP32_HIDRO_AABBCC`) y user `mqtt_esp` **no** recibe el topic de A.
3. Core B **no** puede publicar en `hidrowave/ESP32_HIDRO_1A575C/#`.
4. User `bridge_internal` **sí** recibe `hidrowave/+/relay/state`.
5. UI: usuario email solo ve `device_id` ligados a su `user_email` (RLS). Otro email no ve el Core de bancada.

### Serial Core

- `[MQTT] Connected clientId=ESP32_HIDRO_1A575C`
- `mqtt=1` (no `Failed rc=-2`)

### UI Atlas

- `relay_slaves.last_update` fresco (&lt; 90 s)
- sala 1 **Online** (MAC Atlas `14:33:5C:38:BF:60` vía Core)

## Fuera de alcance

- Scan ESP-NOW / lock canal 5
- PCF8574 (relé físico OFF)
- Firewall Lightsail (arreglar en consola AWS)

## Orden

1. Red 1883 + Mosquitto/bridge `active`
2. (Opcional) ACL `%c` + user `mqtt_esp` en la VM
3. Firmware `mqtt_esp` (paso 2)
4. Flash Core, no Slave
5. Checklist Serial + UI
