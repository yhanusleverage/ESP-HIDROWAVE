# Deploy ACL alignment + bridge to Lightsail
# Uso:
#   .\align-broker-lightsail.ps1 -PemPath "C:\path\key.pem" -DevicePass "SUA_SENHA"
#
# Alinha broker ao master ESP32_HIDRO_1A575C e atualiza bridge.

param(
  [string]$PemPath = "$env:USERPROFILE\Documents\Projects\LightsailDefaultKey-ca-central-1.pem",
  [string]$SshHost = "ubuntu@15.175.109.90",
  [string]$DeviceId = "ESP32_HIDRO_1A575C",
  [Parameter(Mandatory = $true)]
  [string]$DevicePass
)

$ErrorActionPreference = "Stop"
$MosquittoDir = Join-Path (Split-Path $PSScriptRoot -Parent) "mosquitto"
$AlignSh = Join-Path $MosquittoDir "align-broker-production.sh"
$AclProd = Join-Path $MosquittoDir "acl.production"

if (-not (Test-Path $PemPath)) { Write-Error "PEM no encontrado: $PemPath" }
if (-not (Test-Path $AlignSh)) { Write-Error "align-broker-production.sh no encontrado" }

Write-Host ">> SCP align scripts -> VM"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $AlignSh "${SshHost}:/tmp/align-broker-production.sh"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $AclProd "${SshHost}:/tmp/acl.production"

Write-Host ">> Executar align na VM (sudo)"
$escapedPass = $DevicePass -replace "'", "'\''"
ssh -i $PemPath -o StrictHostKeyChecking=accept-new $SshHost @"
sudo cp /tmp/acl.production /tmp/acl.production.bak &&
sudo cp /tmp/align-broker-production.sh /tmp/align-broker-production.sh &&
sudo chmod +x /tmp/align-broker-production.sh &&
sudo bash /tmp/align-broker-production.sh '$DeviceId' '$escapedPass'
"@

Write-Host ">> Deploy bridge (index.js atualizado)"
& (Join-Path $PSScriptRoot "deploy-lightsail.ps1") -PemPath $PemPath -SshHost $SshHost

Write-Host ""
Write-Host "=== Concluído ==="
Write-Host "1. secrets.ini: mqtt_user = mqtt_$DeviceId"
Write-Host "2. pio run -t upload"
Write-Host "3. journalctl -u hidrowave-bridge -f  (deve ver $DeviceId)"
