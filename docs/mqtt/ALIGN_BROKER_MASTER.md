# Alinhar broker MQTT — master `ESP32_HIDRO_1A575C`

**Problema:** ACL na VM amarrava `user hidrowave` só ao device `269844`. O master novo publica em `hidrowave/ESP32_HIDRO_1A575C/#` e era rejeitado.

## Procedimento único

### Opção A — PowerShell (do PC)

```powershell

`
  -DevicePass "MESMA_SENHA_DO_secrets.ini_mqtt_pass"
```

### Opção B — Manual na VM

```bash
# Copiar (do PC):
# scp -i key.pem infra/mqtt/mosquitto/align-broker-production.sh \
#     infra/mqtt/mosquitto/acl.production ubuntu@99.79.36.220:/tmp/

sudo bash /tmp/align-broker-production.sh ESP32_HIDRO_1A575C 'SUA_SENHA_MQTT_DEVICE'
sudo grep -E '^user |topic ' /var/lib/mosquitto/acl
journalctl -u hidrowave-bridge -f
```

### Firmware (já atualizado em `secrets.ini`)

```ini
mqtt_user = mqtt_ESP32_HIDRO_1A575C
mqtt_pass = (mesma senha do align)
```

```powershell
pio run -t upload
pio device monitor
```

## ACL final esperado

| User | Função |
|------|--------|
| `bridge_internal` | read telemetria + `command_ack` + `relay/state` |
| `hidrowave` | Railway: `write hidrowave/+/command` |
| `mqtt_ESP32_HIDRO_1A575C` | ESP: read command, write próprio `#` |

## Verificação

Serial Master:
```
[MQTT] Connected clientId=ESP32_HIDRO_1A575C
[MQTT] heartbeat ...
```

Bridge (não deve ficar só em 269844):
```
[bridge] ... ESP32_HIDRO_1A575C ...
```

Teste comando slave (PC):
```powershell
cd ESP-HIDROWAVE-main\infra\mqtt\bridge
$env:TEST_DEVICE_ID="ESP32_HIDRO_1A575C"
$env:TEST_SLAVE_MAC="14:33:5C:38:BF:60"
$env:MQTT_USER="hidrowave"
$env:MQTT_PASS="(senha Railway)"
npm run test:pub:slave-command
```
