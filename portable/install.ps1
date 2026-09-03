# NInfer portable package - installer
# Registers two scheduled tasks (logon-triggered, highest privilege):
#   NInferServe     -> serve.bat          (the inference service)
#   NInferDraftCtrl -> draft_controller.ps1 (dynamic draft switching)
param([string]$Root = (Split-Path -Parent $MyInvocation.MyCommand.Path))
$ErrorActionPreference = 'Stop'

Write-Host "NInfer portable installer, root: $Root"

# sanity checks
if (-not (Test-Path "$Root\bin\ninfer-serve.exe")) { throw 'bin\ninfer-serve.exe missing' }
$modelLine = (Get-Content "$Root\config\settings.cmd" | Where-Object { $_ -match '^set "NINFER_MODEL=(.*)"$' })
$model = $Matches[1]
if (-not (Test-Path "$Root\$model")) { throw "model missing: $Root\$model" }
$gpu = (& nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>$null)
if (-not $gpu) { throw 'nvidia-smi not found / no NVIDIA GPU' }
Write-Host "GPU: $gpu"

# dirs
New-Item -ItemType Directory -Force -Path "$Root\logs", "$Root\state" | Out-Null

# tasks
schtasks /Create /F /TN NInferServe /TR "`"$Root\serve.bat`"" /SC ONLOGON /RL HIGHEST | Out-Null
schtasks /Create /F /TN NInferDraftCtrl /TR "powershell -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$Root\draft_controller.ps1`"" /SC ONLOGON /RL HIGHEST | Out-Null

# desktop shortcut for the console
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut("$env:USERPROFILE\Desktop\NInfer Console.lnk")
$lnk.TargetPath = "$Root\console.bat"
$lnk.WorkingDirectory = $Root
$lnk.Description = 'NInfer read-only log console'
$lnk.Save()

Write-Host ''
Write-Host 'Installed. Tasks: NInferServe, NInferDraftCtrl (both ONLOGON).'
Write-Host 'Start now:  start.bat    Stop: stop.bat    Status: status.ps1    Logs: console.bat'
Write-Host 'To remove:  uninstall.ps1'
