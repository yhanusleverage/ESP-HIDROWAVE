# Deploy bridge pH — Lightsail (99.79.36.220)

> **Handoff serial:** ver [`docs/handoffs/ph/S07_BRIDGE_MQTT.md`](../docs/handoffs/ph/S07_BRIDGE_MQTT.md)

## Acceso SSH

**Opción A (recomendada):** Lightsail → instancia → **Connect using SSH** (navegador).

**Opción B:** PowerShell con clave `.pem`:

```powershell
ssh -i "C:\Users\THANUS\.ssh\lightsail-key.pem" ubuntu@99.79.36.220
```

## 1. Actualizar bridge

Desde tu PC (copiar `index.js` y test):

```powershell
scp -i "C:\Users\THANUS\.ssh\lightsail-key.pem" `
  "ESP-HIDROWAVE-main\infra\mqtt\bridge\index.js" `
  ubuntu@99.79.36.220:/opt/hidrowave-bridge/

scp -i "C:\Users\THANUS\.ssh\lightsail-key.pem" `
  "ESP-HIDROWAVE-main\infra\mqtt\bridge\scripts\test-publish-ph-dose.js" `
  ubuntu@99.79.36.220:/opt/hidrowave-bridge/scripts/
```

En el servidor:

```bash
cd /opt/hidrowave-bridge
# package.json debe incluir: "test:pub:ph-dose"
sudo systemctl restart hidrowave-bridge
sudo journalctl -u hidrowave-bridge -n 30 --no-pager
```

## 2. ACL Mosquitto

```bash
sudo nano /var/lib/mosquitto/acl
```

Bajo `user bridge_internal`, añadir:

```text
topic read hidrowave/+/ph_operation
topic read hidrowave/+/ph_dose
```

```bash
sudo systemctl restart mosquitto
sudo systemctl restart hidrowave-bridge
```

Referencia: [`ESP-HIDROWAVE-main/infra/mqtt/mosquitto/acl.example`](../../ESP-HIDROWAVE-main/infra/mqtt/mosquitto/acl.example)

## 3. Test sin ESP

```bash
cd /opt/hidrowave-bridge
TEST_DEVICE_ID=ESP32_HIDRO_269844 npm run test:pub:ph-dose
```

## 4. Verificar Supabase

```sql
SELECT ph_operation_state, ph_operation_remaining_sec
FROM relay_master WHERE device_id = 'ESP32_HIDRO_269844';

SELECT * FROM ph_dosages
WHERE device_id = 'ESP32_HIDRO_269844'
ORDER BY created_at DESC LIMIT 3;
```

Reset tras test manual: [`reset-ph-operation.sql`](reset-ph-operation.sql)
