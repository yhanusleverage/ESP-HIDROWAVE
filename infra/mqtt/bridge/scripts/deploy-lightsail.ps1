#Requires -Version 5.1
<#
.SYNOPSIS
  Deploy del MQTT bridge a Lightsail (PowerShell).

.DESCRIPTION
  Fija QUE sube y QUE NO. Evita scp ad-hoc y el bug de $(date) en PowerShell
  (el stamp de backup se calcula en bash remoto).

  SIEMPRE (default):
    index.js
    schedule-evaluator.js
    schedule-mqtt-publish.js
    package.json

  NUNCA:
    .env / .env.local
    node_modules/
    frontend HIDROWAVE / Railway
    firmware ESP
    ACL Mosquitto (salvo -WithAcl)
    scripts de test (salvo -WithTests)

.EXAMPLE
  cd ESP-HIDROWAVE-main\infra\mqtt\bridge
  .\scripts\deploy-lightsail.ps1

.EXAMPLE
  .\scripts\deploy-lightsail.ps1 -DryRun
  .\scripts\deploy-lightsail.ps1 -WithTests
  .\scripts\deploy-lightsail.ps1 -WithAcl
#>

param(
  [string]$PemPath = "$env:USERPROFILE\Documents\Projects\LightsailDefaultKey-ca-central-1.pem",
  [string]$SshHost = "ubuntu@15.175.109.90",
  [string]$RemoteDir = "/opt/hidrowave-bridge",
  [switch]$WithTests,
  [switch]$WithAcl,
  [switch]$NpmInstall,
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$BridgeDir = Split-Path $PSScriptRoot -Parent
$MosquittoDir = Join-Path (Split-Path $BridgeDir -Parent) "mosquitto"

if (-not (Test-Path $PemPath)) {
  Write-Error "PEM no encontrado: $PemPath"
}

# Nunca usar $host (reservado en PowerShell) — solo $SshHost
$sshOpts = @("-i", $PemPath, "-o", "StrictHostKeyChecking=accept-new")

function Write-Section([string]$msg) {
  Write-Host ""
  Write-Host "=== $msg ===" -ForegroundColor Cyan
}

function Invoke-ScpLocal([string]$LocalPath, [string]$RemoteTmpName) {
  if (-not (Test-Path $LocalPath)) {
    Write-Warning "Omitido (no existe): $LocalPath"
    return $false
  }
  $dest = "${SshHost}:/tmp/$RemoteTmpName"
  if ($DryRun) {
    Write-Host "[dry-run] scp $LocalPath -> $dest"
    return $true
  }
  Write-Host "SCP $RemoteTmpName"
  & scp @sshOpts $LocalPath $dest
  if ($LASTEXITCODE -ne 0) { throw "scp failed: $LocalPath" }
  return $true
}

$coreFiles = @(
  @{ Local = (Join-Path $BridgeDir "index.js");                 RemoteTmp = "hw-bridge-index.js";                 RemoteName = "index.js" }
  @{ Local = (Join-Path $BridgeDir "schedule-evaluator.js");    RemoteTmp = "hw-bridge-schedule-evaluator.js";    RemoteName = "schedule-evaluator.js" }
  @{ Local = (Join-Path $BridgeDir "schedule-mqtt-publish.js"); RemoteTmp = "hw-bridge-schedule-mqtt-publish.js"; RemoteName = "schedule-mqtt-publish.js" }
  @{ Local = (Join-Path $BridgeDir "package.json");             RemoteTmp = "hw-bridge-package.json";             RemoteName = "package.json" }
)

Write-Section "Que SUBE a Lightsail (este run)"
$coreFiles | ForEach-Object { Write-Host ("  YES  {0}" -f $_.RemoteName) }
if ($WithTests) { Write-Host "  YES  scripts/test-publish-*.js" }
if ($WithAcl) { Write-Host "  YES  mosquitto ACL patches (CUIDADO)" }
if ($NpmInstall) { Write-Host "  YES  npm install --omit=dev remoto" }

Write-Section "Que NO sube (nunca)"
@(
  ".env / secretos (solo en $RemoteDir/.env del server)",
  "node_modules/",
  "HIDROWAVE frontend / Railway",
  "firmware ESP",
  "docs / agent transcripts"
) | ForEach-Object { Write-Host "  NO   $_" }

Write-Section "Upload core"
$uploaded = @()
foreach ($f in $coreFiles) {
  if (Invoke-ScpLocal $f.Local $f.RemoteTmp) {
    $uploaded += $f
  }
}

if ($uploaded.Count -eq 0) {
  Write-Error "Nada que subir."
}

$testUploads = @()
if ($WithTests) {
  Write-Section "Upload tests"
  $testNames = @(
    "test-publish-ph-dose.js",
    "test-publish-ec-dose.js",
    "test-publish-ec-metric.js",
    "test-publish-ph-metric.js",
    "test-publish-slave-command.js",
    "test-publish-procedure-cmd.js",
    "check-relay-slave-row.js"
  )
  foreach ($n in $testNames) {
    $p = Join-Path $BridgeDir "scripts\$n"
    if (Invoke-ScpLocal $p "hw-bridge-$n") {
      $testUploads += $n
    }
  }
}

$aclUploads = @()
if ($WithAcl) {
  Write-Section "Upload ACL (opt-in)"
  $aclNames = @(
    "patch-acl-dose-topics.sh",
    "patch-acl-metric-topics.sh",
    "patch-acl-hidrowave-publish.sh",
    "align-broker-production.sh"
  )
  foreach ($n in $aclNames) {
    $p = Join-Path $MosquittoDir $n
    if (Invoke-ScpLocal $p "hw-acl-$n") {
      $aclUploads += $n
    }
  }
}

if ($DryRun) {
  Write-Host ""
  Write-Host "[dry-run] No se reinicia servicio." -ForegroundColor Yellow
  exit 0
}

Write-Section "Install + restart hidrowave-bridge"

# Script remoto: STAMP con date de bash (NO PowerShell Get-Date / $(date))
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("set -e")
[void]$sb.AppendLine('STAMP=$(date +%Y%m%d%H%M%S)')
[void]$sb.AppendLine("sudo mkdir -p $RemoteDir $RemoteDir/bak $RemoteDir/scripts")

foreach ($f in $uploaded) {
  [void]$sb.AppendLine("if [ -f $RemoteDir/$($f.RemoteName) ]; then sudo cp `"$RemoteDir/$($f.RemoteName)`" `"$RemoteDir/bak/$($f.RemoteName).`$STAMP`"; fi")
  [void]$sb.AppendLine("sudo cp /tmp/$($f.RemoteTmp) $RemoteDir/$($f.RemoteName)")
}

[void]$sb.AppendLine("sudo chown hidrowave:hidrowave $RemoteDir/index.js $RemoteDir/schedule-evaluator.js $RemoteDir/schedule-mqtt-publish.js $RemoteDir/package.json 2>/dev/null || true")

foreach ($n in $testUploads) {
  [void]$sb.AppendLine("sudo cp /tmp/hw-bridge-$n $RemoteDir/scripts/$n")
}
if ($testUploads.Count -gt 0) {
  [void]$sb.AppendLine("sudo chown -R hidrowave:hidrowave $RemoteDir/scripts")
}

foreach ($n in $aclUploads) {
  [void]$sb.AppendLine("sudo bash /tmp/hw-acl-$n || true")
}

if ($NpmInstall) {
  [void]$sb.AppendLine("cd $RemoteDir && sudo -u hidrowave npm install --omit=dev")
}

[void]$sb.AppendLine("sudo systemctl restart hidrowave-bridge")
[void]$sb.AppendLine("sleep 2")
[void]$sb.AppendLine("systemctl is-active hidrowave-bridge")
[void]$sb.AppendLine("ls -la $RemoteDir/index.js $RemoteDir/schedule-evaluator.js $RemoteDir/schedule-mqtt-publish.js")
[void]$sb.AppendLine("sudo journalctl -u hidrowave-bridge -n 25 --no-pager | tail -n 25")

$remoteScript = $sb.ToString()
# Evitar que PowerShell expanda $STAMP: el heredoc remoto usa bash -s
$remoteScript | & ssh @sshOpts $SshHost "bash -s"
if ($LASTEXITCODE -ne 0) {
  Write-Error "Remote install/restart failed (exit $LASTEXITCODE)."
}

Write-Section "OK"
Write-Host "Runtime en $RemoteDir"
Write-Host "  .\scripts\deploy-lightsail.ps1"
Write-Host "  .\scripts\deploy-lightsail.ps1 -DryRun"
Write-Host "  .\scripts\deploy-lightsail.ps1 -WithTests"
