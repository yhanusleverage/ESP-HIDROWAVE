# Deploy bridge index.js a Lightsail (ubuntu@99.79.36.220)
# Uso: .\scripts\deploy-lightsail.ps1 -PemPath "C:\path\LightsailDefaultKey-ca-central-1.pem"

param(
  [string]$PemPath = "$env:USERPROFILE\Documents\Projects\LightsailDefaultKey-ca-central-1.pem",
  [string]$Host = "ubuntu@99.79.36.220"
)

$ErrorActionPreference = "Stop"
$BridgeDir = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$IndexJs = Join-Path $BridgeDir "index.js"

if (-not (Test-Path $PemPath)) {
  Write-Error "PEM no encontrado: $PemPath"
}
if (-not (Test-Path $IndexJs)) {
  Write-Error "index.js no encontrado: $IndexJs"
}

Write-Host ">> SCP index.js -> $Host:~/index.js"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $IndexJs "${Host}:~/index.js"

Write-Host ">> Instalar y reiniciar hidrowave-bridge"
ssh -i $PemPath -o StrictHostKeyChecking=accept-new $Host @"
sudo cp ~/index.js /opt/hidrowave-bridge/index.js
sudo systemctl restart hidrowave-bridge
sleep 2
sudo journalctl -u hidrowave-bridge -n 15 --no-pager | grep -E 'Subscribed|ec_operation|dose|error' || true
"@

Write-Host ">> Deploy completado"
