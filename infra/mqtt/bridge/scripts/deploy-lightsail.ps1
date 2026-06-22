# Deploy bridge index.js a Lightsail (ubuntu@99.79.36.220)
# Uso: .\scripts\deploy-lightsail.ps1 -PemPath "C:\path\LightsailDefaultKey-ca-central-1.pem"

param(
  [string]$PemPath = "$env:USERPROFILE\Documents\Projects\LightsailDefaultKey-ca-central-1.pem",
  [string]$SshHost = "ubuntu@99.79.36.220"
)

$ErrorActionPreference = "Stop"
$BridgeDir = Split-Path $PSScriptRoot -Parent
$IndexJs = Join-Path $BridgeDir "index.js"
$PhTestJs = Join-Path $BridgeDir "scripts\test-publish-ph-dose.js"
$EcTestJs = Join-Path $BridgeDir "scripts\test-publish-ec-dose.js"
$EcMetricJs = Join-Path $BridgeDir "scripts\test-publish-ec-metric.js"
$PhMetricJs = Join-Path $BridgeDir "scripts\test-publish-ph-metric.js"
$PackageJson = Join-Path $BridgeDir "package.json"
$AclDosePatch = Join-Path (Split-Path $BridgeDir -Parent) "mosquitto\patch-acl-dose-topics.sh"
$AclMetricPatch = Join-Path (Split-Path $BridgeDir -Parent) "mosquitto\patch-acl-metric-topics.sh"
$AclHidrowavePatch = Join-Path (Split-Path $BridgeDir -Parent) "mosquitto\patch-acl-hidrowave-publish.sh"
$SlaveCmdJs = Join-Path $BridgeDir "scripts\test-publish-slave-command.js"

if (-not (Test-Path $PemPath)) {
  Write-Error "PEM no encontrado: $PemPath"
}
if (-not (Test-Path $IndexJs)) {
  Write-Error "index.js no encontrado: $IndexJs"
}

Write-Host ">> SCP bridge files -> ${SshHost}:/tmp/"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $IndexJs "${SshHost}:/tmp/hidrowave-index.js"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $PhTestJs "${SshHost}:/tmp/test-publish-ph-dose.js"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $EcTestJs "${SshHost}:/tmp/test-publish-ec-dose.js"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $EcMetricJs "${SshHost}:/tmp/test-publish-ec-metric.js"
scp -i $PemPath -o StrictHostKeyChecking=accept-new $PhMetricJs "${SshHost}:/tmp/test-publish-ph-metric.js"
if (Test-Path $AclDosePatch) {
  scp -i $PemPath -o StrictHostKeyChecking=accept-new $AclDosePatch "${SshHost}:/tmp/patch-acl-dose-topics.sh"
}
if (Test-Path $AclMetricPatch) {
  scp -i $PemPath -o StrictHostKeyChecking=accept-new $AclMetricPatch "${SshHost}:/tmp/patch-acl-metric-topics.sh"
}
if (Test-Path $AclHidrowavePatch) {
  scp -i $PemPath -o StrictHostKeyChecking=accept-new $AclHidrowavePatch "${SshHost}:/tmp/patch-acl-hidrowave-publish.sh"
}
if (Test-Path $SlaveCmdJs) {
  scp -i $PemPath -o StrictHostKeyChecking=accept-new $SlaveCmdJs "${SshHost}:/tmp/test-publish-slave-command.js"
}
scp -i $PemPath -o StrictHostKeyChecking=accept-new $PackageJson "${SshHost}:/tmp/hidrowave-package.json"

Write-Host ">> Instalar y reiniciar hidrowave-bridge"
$remoteCmd = @"
sudo cp /tmp/hidrowave-index.js /opt/hidrowave-bridge/index.js &&
sudo mkdir -p /opt/hidrowave-bridge/scripts &&
sudo cp /tmp/test-publish-ph-dose.js /opt/hidrowave-bridge/scripts/test-publish-ph-dose.js &&
sudo cp /tmp/test-publish-ec-dose.js /opt/hidrowave-bridge/scripts/test-publish-ec-dose.js &&
sudo cp /tmp/test-publish-ec-metric.js /opt/hidrowave-bridge/scripts/test-publish-ec-metric.js &&
sudo cp /tmp/test-publish-ph-metric.js /opt/hidrowave-bridge/scripts/test-publish-ph-metric.js &&
if [ -f /tmp/test-publish-slave-command.js ]; then sudo cp /tmp/test-publish-slave-command.js /opt/hidrowave-bridge/scripts/test-publish-slave-command.js; fi &&
sudo cp /tmp/hidrowave-package.json /opt/hidrowave-bridge/package.json &&
sudo chmod 755 /opt/hidrowave-bridge/scripts &&
if [ -f /tmp/patch-acl-dose-topics.sh ]; then sudo bash /tmp/patch-acl-dose-topics.sh; fi &&
if [ -f /tmp/patch-acl-metric-topics.sh ]; then sudo bash /tmp/patch-acl-metric-topics.sh; fi &&
if [ -f /tmp/patch-acl-hidrowave-publish.sh ]; then sudo bash /tmp/patch-acl-hidrowave-publish.sh; fi &&
sudo chown hidrowave:hidrowave /opt/hidrowave-bridge/scripts/*.js &&
sudo systemctl restart hidrowave-bridge &&
sleep 2 &&
sudo journalctl -u hidrowave-bridge -n 20 --no-pager | grep -E 'Subscribed|dose|ph_dose|ec_metric|ph_metric|INSERT|error' || true
"@
ssh -i $PemPath -o StrictHostKeyChecking=accept-new $SshHost $remoteCmd

Write-Host ">> Deploy completado"
Write-Host ">> Gates pos-deploy (en servidor /opt/hidrowave-bridge):"
Write-Host "   npm run test:pub:ec-dose    # R1 regresion"
Write-Host "   npm run test:pub:ph-dose    # R2 regresion"
Write-Host "   npm run test:pub:ec-metric  # V3"
Write-Host "   npm run test:pub:ph-metric  # V4"
Write-Host "   npm run test:pub:slave-command  # slave ESP-NOW (Fase 3)"
