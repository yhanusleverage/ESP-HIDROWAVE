# ACL Lightsail + mapa MQTT — 27 ago 2026

**Broker:** `15.175.109.90:1883` (VM `ip-172-26-12-101`)  
**ACL vivo:** `/var/lib/mosquitto/acl`  
**Cores:** `ESP32_HIDRO_1A575C` (bancada) · `ESP32_HIDRO_269844`  
**Users Mosquitto:** `bridge_internal` · `hidrowave` (Railway) · `mqtt_<device_id>`  
**Relacionado:** [HANDOFF_ACL_FLEET_VALIDATION.md](HANDOFF_ACL_FLEET_VALIDATION.md) · [HANDOFF_UPLOAD_LIGHTSAIL.md](HANDOFF_UPLOAD_LIGHTSAIL.md) · transporte Auto EC/pH (HIDROWAVE-main `docs/handoffs/ec/`)

Postgres (views UI) **sigue siendo la fuente de verdad**. MQTT es el tubo al ESP.

---

## 1. Foto del ACL en la VM (antes de config Auto)

Pegado 27/08 ~10:15 UTC-3. **Incompleto para Auto EC/pH.** Relés `command` sí.

```
user hidrowave
topic write hidrowave/+/command
topic read hidrowave/+/#

user mqtt_ESP32_HIDRO_1A575C
topic read hidrowave/ESP32_HIDRO_1A575C/command
topic write hidrowave/ESP32_HIDRO_1A575C/#
```

(`1A575C` estaba **duplicado**; Mosquitto une reglas, pero hay que dejar un solo bloque.)

`write hidrowave/{id}/#` **no** da **subscribe** a `ec/config`. Hace falta `topic read …/ec/config`.

---

## 2. ACL objetivo (aplicar en nano)

```
# --- Bridge Node ---
user bridge_internal
topic read hidrowave/+/telemetry
topic read hidrowave/+/heartbeat
topic read hidrowave/+/status
topic read hidrowave/+/ec_operation
topic read hidrowave/+/dose
topic read hidrowave/+/ph_operation
topic read hidrowave/+/ph_dose
topic read hidrowave/+/ec_metric
topic read hidrowave/+/ph_metric
topic read hidrowave/+/ec_gain
topic read hidrowave/+/ph_gain
topic read hidrowave/+/ec_dilution
topic read hidrowave/+/command_ack
topic read hidrowave/+/relay/state
topic read hidrowave/+/levels

# --- Railway / UI ---
user hidrowave
topic write hidrowave/+/command
topic write hidrowave/+/ec/config
topic write hidrowave/+/ph/config
topic read hidrowave/+/#

# --- Master ESP32_HIDRO_1A575C ---
user mqtt_ESP32_HIDRO_1A575C
topic read hidrowave/ESP32_HIDRO_1A575C/command
topic read hidrowave/ESP32_HIDRO_1A575C/ec/config
topic read hidrowave/ESP32_HIDRO_1A575C/ph/config
topic write hidrowave/ESP32_HIDRO_1A575C/#

# --- Master ESP32_HIDRO_269844 ---
user mqtt_ESP32_HIDRO_269844
topic read hidrowave/ESP32_HIDRO_269844/command
topic read hidrowave/ESP32_HIDRO_269844/ec/config
topic read hidrowave/ESP32_HIDRO_269844/ph/config
topic write hidrowave/ESP32_HIDRO_269844/#
```

Tras guardar:

```bash
sudo systemctl restart mosquitto
sudo systemctl is-active mosquitto
sudo grep -n 'ec/config' /var/lib/mosquitto/acl
```

Repo alineado: [acl.example](../../infra/mqtt/mosquitto/acl.example) · [acl.production](../../infra/mqtt/mosquitto/acl.production) · [patch-acl-config-topics.sh](../../infra/mqtt/mosquitto/patch-acl-config-topics.sh) (si el scp viene de Windows: `sed -i 's/\r$//' /tmp/patch-acl-config-topics.sh` antes de `sudo bash`).

---

## 3. Registro de implementaciones

| Fecha | Qué | Dónde | Estado |
|-------|-----|--------|--------|
| jun/2026 | MQTT telemetría + heartbeat + LWT | ESP `MqttClient` + bridge | Hecho |
| jun/2026 | Relés `…/command` + ACK | UI Railway `hidrowave` write command; ESP subscribe | Hecho |
| 22/08 | ACL flota / `%c` (diseño) | HANDOFF_ACL_FLEET_VALIDATION | Diseño; prod sigue user por device |
| 25/08 | Fase A: `mqtt_health_only=1`; skip PATCH `relay_master` si MQTT OK | firmware | Hecho |
| 27/08 | GET config skip `maxAlloc=38900` | bancada | GET **no** es canal |
| 27/08 | Save UI: no columna `flow_rate` | `controller-config-api.ts` | Hecho |
| 27/08 | Publish retained `ec/config` + `ph/config` post-UPSERT | `mqtt-config-publish.ts` + API EC/pH | Código; falta ACL VM + flash |
| 27/08 | ESP `incomingHandler`: command vs config por tópico | `MqttClient` + `HydroSystemCore` | Código; falta flash |
| 27/08 | ACL VM `ec/config` | `/var/lib/mosquitto/acl` | **Pendiente** (foto §1) |

---

## 4. Mapa de funcionalidad

```text
UI / Railway                         Broker                     ESP / bridge
────────────                         ──────                     ────────────
Relé ON/OFF     ──pub QoS1──►  …/{id}/command     ──sub──►  malha relé
Guardar Auto EC ──pub retain──► …/{id}/ec/config  ──sub──►  RAM auto EC
Guardar Auto pH ──pub retain──► …/{id}/ph/config  ──sub──►  RAM auto pH
Sensores        ──pub───────►  …/telemetry         ──► bridge INSERT
EC/pH operación ──pub───────►  …/ec_operation     ──► relay_master
Dosis           ──pub───────►  …/dose             ──► nutrient_dosages
Heartbeat       ──pub───────►  …/heartbeat         ──► device_status
```

| Función | Tópico / vía | Quién publica | Quién consume | Estado |
|---------|--------------|---------------|---------------|--------|
| Relé comando | `hidrowave/{id}/command` | Railway `hidrowave` | ESP | **Prod** |
| Auto EC config | `hidrowave/{id}/ec/config` retained | API tras UPSERT view | ESP | Código; **ACL+flash** |
| Auto pH config | `hidrowave/{id}/ph/config` retained | API | ESP | Código; **ACL+flash** |
| GET HTTPS `ec_config_view` | REST TLS | ESP poll | RAM | Fallback; **sordo** si maxAlloc &lt; 40 KB |
| Telemetría | `…/telemetry` | ESP | bridge | Prod |
| Operación EC/pH | `…/ec_operation` `…/ph_operation` | ESP | bridge | Prod |
| Dosis / métricas | `…/dose` `…/ec_metric` | ESP | bridge | Prod |
| Presencia | `…/heartbeat` `…/status` LWT | ESP | bridge | Prod; HTTPS last_seen ≤4 min |
| Niveles | `…/levels` | ESP | bridge | Prod |
| Relé estado | `…/relay/state` | ESP | bridge | Prod |
| `decision_rules` | HTTPS poll ~30 s | ESP | LittleFS | **Siguiente** cuello TLS |
| Claim / registro | HTTPS 1× | ESP | `device_status` | Queda HTTPS |

`incomingHandler` **no** sustituye relés: mismo `handleMqttCommandPayload` si el tópico no es `ec/config` ni `ph/config`.

---

## 4b. Incidente 28/08/2026 — typo `hidrowa_ve/+/command`

**Síntoma:** `[MQTT CMD] published` na UI (id 1350/1351), ESP com `mqtt=1`, serial **sem** `[MQTT] rx topic` / `[CMD mqtt]`.

**Causa:** ACL user `hidrowave` com typo:

```diff
- topic write hidrowa_ve/+/command
+ topic write hidrowave/+/command
```

EC/pH config (`hidrowave/+/ec/config`) continuavam OK — só comandos de relé (cebar, manual) falhavam.

**Fix:** corrigir linha no ACL + `sudo systemctl restart mosquitto`.

Guia debug completo: [MQTT_COMANDOS_DEBUG_ACL.md](../../../HIDROWAVE-main/docs/MQTT_COMANDOS_DEBUG_ACL.md).

---

## 5. Gate bancada (cuando ACL + firmware estén)

Serial ESP:

```
[MQTT] subscribe command QoS1 hidrowave/ESP32_HIDRO_1A575C/command
[MQTT] subscribe ec/config QoS1 hidrowave/ESP32_HIDRO_1A575C/ec/config
[MQTT] rx topic=hidrowave/ESP32_HIDRO_1A575C/ec/config
[EC CONFIG] apply via=mqtt auto=SIM
```

UI: Guardar parámetros Auto EC **sin** error `flow_rate`. Toggle Auto pH → serial `auto=SIM` sin `GET skip maxAllocLow`.