# Lightsail bridge — qué sube / qué no (PowerShell)

**Script:** [`infra/mqtt/bridge/scripts/deploy-lightsail.ps1`](../../infra/mqtt/bridge/scripts/deploy-lightsail.ps1)

## Uso

```powershell
cd "ESP-HIDROWAVE-main\infra\mqtt\bridge"
.\scripts\deploy-lightsail.ps1              # solo runtime (recomendado)
.\scripts\deploy-lightsail.ps1 -DryRun      # lista sin tocar server
.\scripts\deploy-lightsail.ps1 -WithTests   # + scripts test
.\scripts\deploy-lightsail.ps1 -WithAcl     # + patches Mosquitto (cuidado)
```

- PEM default: `%USERPROFILE%\Documents\Projects\LightsailDefaultKey-ca-central-1.pem`
- Host: `ubuntu@15.175.109.90` → `/opt/hidrowave-bridge`
- Backup remoto: `/opt/hidrowave-bridge/bak/<file>.<stamp>` (stamp en **bash**, no PowerShell)

## SÍ sube (default)

| Archivo | Para qué |
|---------|----------|
| `index.js` | Bridge principal |
| `schedule-evaluator.js` | Alarmas / Ativar |
| `schedule-mqtt-publish.js` | Upsert + procedure/cmd |
| `package.json` | Metadata (`-NpmInstall` si hace falta deps) |

## NO sube nunca

- `.env` (secretos solo en el server)
- `node_modules/`
- Frontend HIDROWAVE / Railway
- Firmware ESP
- ACL Mosquitto (solo `-WithAcl`)

## Tras deploy

```powershell
ssh -i $pem ubuntu@15.175.109.90 "systemctl is-active hidrowave-bridge"
```

**Nota:** en PowerShell usar `$sshHost`, nunca `$host` (variable reservada).
