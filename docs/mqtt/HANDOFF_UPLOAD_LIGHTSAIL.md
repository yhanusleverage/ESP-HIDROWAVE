# Handoff — subir ACL + bridge (`index.js`) a Lightsail

**Fecha:** 27 ago 2026  
**Por qué a mano falló:** el `.sh` copiado desde Windows llega con **CRLF**. Bash muere en `set -o pipefail`. Mosquitto **no** lee el git: solo `/var/lib/mosquitto/acl`.

**Broker:** `ubuntu@15.175.109.90`  
**Key:** `C:\Users\THANUS\Documents\Projects\CREDENCIAIS-SSH-AWS-KEY\LightsailDefaultKey-ca-central-1.pem`

| Archivo local | Destino en la VM |
|---------------|-------------------|
| `infra/mqtt/mosquitto/acl.production` + bloques device | `/var/lib/mosquitto/acl` |
| `infra/mqtt/mosquitto/patch-acl-config-topics.sh` | `/tmp/` → `sudo bash` |
| `infra/mqtt/bridge/index.js` | `/opt/hidrowave-bridge/index.js` |

**Nunca** pongas la `.pem` ni passwords en git.

---

## 0. PowerShell — variables (pegar primero)

```powershell
$pem = "C:\Users\THANUS\Documents\Projects\CREDENCIAIS-SSH-AWS-KEY\LightsailDefaultKey-ca-central-1.pem"
$host = "ubuntu@15.175.109.90"
$root = "C:\Users\THANUS\Documents\Projects\ESP-NEW_HOPE - FRONTEND - BACKUP -\ESP-HIDROWAVE-main"

# SSH en Windows: clave no puede ser leída por otros
icacls $pem /inheritance:r
icacls $pem /grant:r "${env:USERNAME}:R"
```

Probar:

```powershell
ssh -i $pem -o StrictHostKeyChecking=accept-new $host "hostname && echo OK"
```

Si pide `yes/no` → `yes`.

---

## 1. Subir y aplicar ACL (config Auto EC/pH)

### A) Script (recomendado)

**En PowerShell:**

```powershell
scp -i $pem -o StrictHostKeyChecking=accept-new `
  "$root\infra\mqtt\mosquitto\patch-acl-config-topics.sh" `
  "${host}:/tmp/patch-acl-config-topics.sh"
```

**En la VM** (`ssh -i $pem $host`):

```bash
# CRLF de Windows → LF
sed -i 's/\r$//' /tmp/patch-acl-config-topics.sh
sudo bash /tmp/patch-acl-config-topics.sh
sudo systemctl restart mosquitto
sudo systemctl is-active mosquitto
sudo grep -n 'ec/config\|ph/config' /var/lib/mosquitto/acl
```

Tenés que ver `write hidrowave/+/ec/config` y `read …/ESP32_HIDRO_1A575C/ec/config`.

Si el script no añade los `read` del device, usá **B**.

### B) Archivo ACL entero (nano)

```bash
sudo cp /var/lib/mosquitto/acl /var/lib/mosquitto/acl.bak.$(date +%Y%m%d%H%M)
sudo nano /var/lib/mosquitto/acl
```

Pegar (sin línea suelta `75C/#`, **un** bloque por device):

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

Ctrl+O, Enter, Ctrl+X:

```bash
sudo systemctl restart mosquitto
sudo systemctl is-active mosquitto
```

`write hidrowave/{id}/#` del ESP **no** alcanza para **leer** `ec/config`.

---

## 2. Subir `index.js` (bridge Node)

**PowerShell:**

```powershell
scp -i $pem -o StrictHostKeyChecking=accept-new `
  "$root\infra\mqtt\bridge\index.js" `
  "${host}:/tmp/hidrowave-index.js"
```

**VM:**

```bash
sudo cp /opt/hidrowave-bridge/index.js /opt/hidrowave-bridge/index.js.bak.$(date +%Y%m%d%H%M)
sudo cp /tmp/hidrowave-index.js /opt/hidrowave-bridge/index.js
sudo chown hidrowave:hidrowave /opt/hidrowave-bridge/index.js
sudo systemctl restart hidrowave-bridge
sudo systemctl is-active hidrowave-bridge
sudo journalctl -u hidrowave-bridge -n 30 --no-pager
```

El bridge **no** necesita tópicos `ec/config` (eso es Railway → ESP). Reiniciarlo no aplica el ACL; **Mosquitto** sí.

Atajo: `.\infra\mqtt\bridge\scripts\deploy-lightsail.ps1 -PemPath $pem`

---

## 3. Un bloque: ACL + index (copiar todo)

PowerShell:

```powershell
$pem = "C:\Users\THANUS\Documents\Projects\CREDENCIAIS-SSH-AWS-KEY\LightsailDefaultKey-ca-central-1.pem"
$host = "ubuntu@15.175.109.90"
$root = "C:\Users\THANUS\Documents\Projects\ESP-NEW_HOPE - FRONTEND - BACKUP -\ESP-HIDROWAVE-main"

scp -i $pem "$root\infra\mqtt\mosquitto\patch-acl-config-topics.sh" "${host}:/tmp/patch-acl-config-topics.sh"
scp -i $pem "$root\infra\mqtt\bridge\index.js" "${host}:/tmp/hidrowave-index.js"

ssh -i $pem $host
```

VM:

```bash
sed -i 's/\r$//' /tmp/patch-acl-config-topics.sh
sudo bash /tmp/patch-acl-config-topics.sh
sudo systemctl restart mosquitto

sudo cp /opt/hidrowave-bridge/index.js /opt/hidrowave-bridge/index.js.bak.$(date +%Y%m%d%H%M)
sudo cp /tmp/hidrowave-index.js /opt/hidrowave-bridge/index.js
sudo chown hidrowave:hidrowave /opt/hidrowave-bridge/index.js
sudo systemctl restart hidrowave-bridge

sudo systemctl is-active mosquitto hidrowave-bridge
sudo grep -n 'ec/config' /var/lib/mosquitto/acl
```

---

## 4. Qué no hace falta

| Cosa | ¿Reiniciar? |
|-------|-------------|
| Instancia Lightsail | No |
| `hidrowave-bridge` por solo ACL | No |
| Mosquitto tras editar ACL | **Sí** (`restart`) |
| ESP | Flash firmware nuevo + reconecta MQTT |
| Railway | Mismas vars `MQTT_HOST` / `MQTT_PUBLISH_USER=hidrowave` |

---

## 5. Gate

```text
sudo grep ec/config /var/lib/mosquitto/acl     → write + y read por device
serial ESP: subscribe ec/config
Guardar Auto EC en web → [MQTT] rx …/ec/config → apply via=mqtt
```

Mapa de tópicos: [ACL_MAPA_FUNCIONALIDADES_27AGO2026.md](./ACL_MAPA_FUNCIONALIDADES_27AGO2026.md)
