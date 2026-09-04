# Handoff — subir ACL + bridge (`index.js`) a Lightsail

**Última validación:** 28 ago 2026 — **deploy OK de primera** (PowerShell + SCP + SSH + VM).  
**Fecha doc original:** 27 ago 2026

**Por qué a mano falló antes:** el `.sh` copiado desde Windows llega con **CRLF**. Bash muere en `set -o pipefail`. Mosquitto **no** lee el git: solo `/var/lib/mosquitto/acl`.

**Broker:** `ubuntu@15.175.109.90` (VM `ip-172-26-12-101`)  
**Key (validada):** `C:\Users\THANUS\Documents\Projects\LightsailDefaultKey-ca-central-1.pem`  
*(alternativa antigua:* `CREDENCIAIS-SSH-AWS-KEY\LightsailDefaultKey-ca-central-1.pem`*)*

| Archivo local | Destino en la VM |
|---------------|-------------------|
| `infra/mqtt/mosquitto/patch-acl-config-topics.sh` | `/tmp/` → `sudo bash` |
| `infra/mqtt/bridge/index.js` | `/opt/hidrowave-bridge/index.js` |
| `infra/mqtt/mosquitto/acl.production` | `/var/lib/mosquitto/acl` (opcional, deploy completo) |

**Nunca** pongas la `.pem` ni passwords en git.

---

## ⚠️ PowerShell: NO uses `$host`

`$Host` es **variable reservada** de PowerShell. Si escribes `$host = "ubuntu@..."` falla con:

```text
VariableNotWritable / SessionStateUnauthorizedAccessException
```

Y `scp`/`ssh` intentará conectar a `System.Management.Automation.Internal.Host.InternalHost` → *Host desconocido*.

**Usa siempre `$sshHost`** (o `$remote`).

---

## 0. PowerShell — variables (pegar primero)

```powershell
$pem = "C:\Users\THANUS\Documents\Projects\LightsailDefaultKey-ca-central-1.pem"
if (-not (Test-Path $pem)) {
  $pem = "C:\Users\THANUS\Documents\Projects\LightsailDefaultKey-ca-central-1"
}

$sshHost = "ubuntu@15.175.109.90"
$root = "C:\Users\THANUS\Documents\Projects\ESP-NEW_HOPE - FRONTEND - BACKUP -\ESP-HIDROWAVE-main"

icacls $pem /inheritance:r
icacls $pem /grant:r "${env:USERNAME}:R"
```

Probar SSH:

```powershell
ssh -i $pem -o StrictHostKeyChecking=accept-new $sshHost "hostname && echo OK"
```

Respuesta esperada: `ip-172-26-12-101` + `OK`. Si pide `yes/no` → `yes`.

Subir archivos:

```powershell
scp -i $pem -o StrictHostKeyChecking=accept-new `
  "$root\infra\mqtt\mosquitto\patch-acl-config-topics.sh" `
  "${sshHost}:/tmp/patch-acl-config-topics.sh"

scp -i $pem -o StrictHostKeyChecking=accept-new `
  "$root\infra\mqtt\bridge\index.js" `
  "${sshHost}:/tmp/hidrowave-index.js"
```

Entrar na VM:

```powershell
ssh -i $pem $sshHost
```

---

## 1. VM — aplicar ACL + bridge (bloque validado 28/08/2026)

Copiar y pegar **en bash** (después del `scp`):

```bash
sed -i 's/\r$//' /tmp/patch-acl-config-topics.sh
sudo bash /tmp/patch-acl-config-topics.sh
sudo systemctl restart mosquitto

sudo cp /opt/hidrowave-bridge/index.js /opt/hidrowave-bridge/index.js.bak.$(date +%Y%m%d%H%M)
sudo cp /tmp/hidrowave-index.js /opt/hidrowave-bridge/index.js
sudo chown hidrowave:hidrowave /opt/hidrowave-bridge/index.js
sudo systemctl restart hidrowave-bridge

sudo systemctl is-active mosquitto hidrowave-bridge
sudo journalctl -u hidrowave-bridge -n 30 --no-pager
```

### Salida real OK (28 ago 2026 ~03:37 UTC)

```text
ACL config topics atualizado.
Mosquitto recarregado.
active
active
Aug 28 02:21:13 ... [bridge] PATCH relay_master ESP32_HIDRO_1A575C ec_operation=idle ...
Aug 28 02:21:13 ... [bridge] PATCH device_status ESP32_HIDRO_1A575C water_level=...
Aug 28 03:37:34 ... systemd[1]: Stopped hidrowave-bridge.service ...
Aug 28 03:37:34 ... systemd[1]: Started hidrowave-bridge.service - HIDROWAVE MQTT to Supabase Bridge.
```

Gate ACL config:

```bash
sudo grep -n 'ec/config\|ph/config' /var/lib/mosquitto/acl
```

Tenés que ver `write hidrowave/+/ec/config` y `read …/ESP32_HIDRO_1A575C/ec/config`.

**ec_gain / ph_gain** — el script `patch-acl-config-topics.sh` **no** los añade. Usar **§2** abajo.

---

## 2. ec_gain / ph_gain (K aprendido MQTT → bridge)

`patch-acl-config-topics.sh` solo añade `ec/config` y `ph/config`. Por eso `grep ec_gain` devuelve **vacío** — es normal.

### Opción A — script (recomendado)

**PowerShell:**

```powershell
scp -i $pem -o StrictHostKeyChecking=accept-new `
  "$root\infra\mqtt\mosquitto\patch-acl-gain-topics.sh" `
  "${sshHost}:/tmp/patch-acl-gain-topics.sh"
```

**VM:**

```bash
sed -i 's/\r$//' /tmp/patch-acl-gain-topics.sh
sudo bash /tmp/patch-acl-gain-topics.sh
sudo grep -n 'ec_gain\|ph_gain' /var/lib/mosquitto/acl
sudo systemctl is-active mosquitto
```

Esperado:

```text
ACL atualizado com ec_gain + ph_gain.
topic read hidrowave/+/ec_gain
topic read hidrowave/+/ph_gain
```

### Opción B — nano manual

```bash
sudo cp /var/lib/mosquitto/acl /var/lib/mosquitto/acl.bak.$(date +%Y%m%d%H%M)
sudo nano /var/lib/mosquitto/acl
```

Dentro del bloque `user bridge_internal`, **después** de `ph_metric` (o junto a otros `topic read`), añadir:

```text
topic read hidrowave/+/ec_gain
topic read hidrowave/+/ph_gain
```

Guardar: Ctrl+O, Enter, Ctrl+X.

```bash
sudo systemctl restart mosquitto
sudo grep -n 'ec_gain\|ph_gain' /var/lib/mosquitto/acl
```

---

## 2b. circ/config + rules/# (tipagem / Motor → Core)

`write hidrowave/{id}/#` del ESP **no** da **subscribe**. Railway tampoco puede publicar `circ/config` ni `rules/#` sin write explícito.

### Opción A — script (recomendado)

**PowerShell:**

```powershell
scp -i $pem -o StrictHostKeyChecking=accept-new `
  "$root\infra\mqtt\mosquitto\patch-acl-rules-circ.sh" `
  "${sshHost}:/tmp/patch-acl-rules-circ.sh"
```

**VM:**

```bash
sed -i 's/\r$//' /tmp/patch-acl-rules-circ.sh
sudo bash /tmp/patch-acl-rules-circ.sh
sudo grep -n 'circ/config\|rules' /var/lib/mosquitto/acl
sudo systemctl is-active mosquitto
```

Esperado: líneas `write hidrowave/+/circ/config`, `write hidrowave/+/rules/#`, y `read …/circ/config` + `read …/rules/#` por cada master.

### Opción B — nano manual

En bloque `user hidrowave`, después de `ph/config`:

```text
topic write hidrowave/+/circ/config
topic write hidrowave/+/rules/#
```

En **cada** master, antes del `write …/#`:

```text
topic read hidrowave/ESP32_HIDRO_XXXXXX/circ/config
topic read hidrowave/ESP32_HIDRO_XXXXXX/rules/#
```

Luego: `sudo systemctl restart mosquitto`.

---

## 3. Verificar bridge / gain (sin cambiar ACL de tipagem)

Tras el patch de gains (§2), confirmar:

```bash
sudo systemctl restart mosquitto
sudo grep -n ec_gain /var/lib/mosquitto/acl
```

**ESP:** no hace falta línea extra si ya tiene `topic write hidrowave/ESP32_HIDRO_1A575C/#` (el `#` incluye `ec_gain` y `ph_gain`).

Tras subir `index.js` con handlers gain, test:

```bash
cd /opt/hidrowave-bridge
TEST_DEVICE_ID=ESP32_HIDRO_1A575C TEST_K_VALUE=0.3721 node scripts/test-publish-ec-gain.js
sudo journalctl -u hidrowave-bridge -n 5 --no-pager | grep ec_gain
```

Esperado: `[bridge] ec_gain PATCH ec_config_view ... k_value=...`

---

## 3b. Atajo script (solo index.js)

```powershell
cd "$root\infra\mqtt\bridge\scripts"
.\deploy-lightsail.ps1 -PemPath $pem
```

*(El script usa `$SshHost`, no `$host`.)*

---

## 4. ACL entero manual (nano) — alternativa

Si `patch-acl-config-topics.sh` no basta, pegar en `/var/lib/mosquitto/acl` (incluir **ec_gain/ph_gain**):

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
topic write hidrowave/+/circ/config
topic write hidrowave/+/rules/#
topic read hidrowave/+/#

# --- Master ESP32_HIDRO_1A575C ---
user mqtt_ESP32_HIDRO_1A575C
topic read hidrowave/ESP32_HIDRO_1A575C/command
topic read hidrowave/ESP32_HIDRO_1A575C/ec/config
topic read hidrowave/ESP32_HIDRO_1A575C/ph/config
topic read hidrowave/ESP32_HIDRO_1A575C/circ/config
topic read hidrowave/ESP32_HIDRO_1A575C/rules/#
topic write hidrowave/ESP32_HIDRO_1A575C/#

# --- Master ESP32_HIDRO_269844 ---
user mqtt_ESP32_HIDRO_269844
topic read hidrowave/ESP32_HIDRO_269844/command
topic read hidrowave/ESP32_HIDRO_269844/ec/config
topic read hidrowave/ESP32_HIDRO_269844/ph/config
topic read hidrowave/ESP32_HIDRO_269844/circ/config
topic read hidrowave/ESP32_HIDRO_269844/rules/#
topic write hidrowave/ESP32_HIDRO_269844/#
```

`write hidrowave/{id}/#` del ESP **no** alcanza para **leer** `ec/config`, `circ/config` ni `rules/#`.

---

## 5. Qué reiniciar

| Cosa | ¿Reiniciar? |
|------|-------------|
| Instancia Lightsail | No |
| `hidrowave-bridge` por solo ACL | No |
| Mosquitto tras editar ACL | **Sí** (`restart`) |
| ESP | Flash firmware nuevo + reconecta MQTT |
| Railway | Mismas vars `MQTT_HOST` / `MQTT_PUBLISH_USER=hidrowave` |

---

## 6. Gate end-to-end

```text
sudo systemctl is-active mosquitto hidrowave-bridge  → active active
sudo grep ec/config /var/lib/mosquitto/acl           → write + read por device
sudo grep 'circ/config\|rules' /var/lib/mosquitto/acl → write Railway + read por device
serial ESP: subscribe ec/config + circ/config + rules/#
Guardar Auto EC en web → [MQTT] rx …/ec/config → apply via=mqtt
Tipagem / Resync ↻ → [MQTT] rules upsert … ok
Auto EC aprende K → [MQTT] ec_gain → bridge PATCH k_value
```

Journal en vivo: [11_ANALISE_BROKER_JOURNAL.md](./11_ANALISE_BROKER_JOURNAL.md)

Mapa de tópicos: [ACL_MAPA_FUNCIONALIDADES_27AGO2026.md](./ACL_MAPA_FUNCIONALIDADES_27AGO2026.md)
