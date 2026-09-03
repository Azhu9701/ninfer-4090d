# NInfer portable package - uninstaller (tasks + desktop shortcut; keeps logs/model/cache)
param([string]$Root = (Split-Path -Parent $MyInvocation.MyCommand.Path))
$ErrorActionPreference = 'Continue'

schtasks /End /TN NInferDraftCtrl 2>$null | Out-Null
schtasks /End /TN NInferServe 2>$null | Out-Null
schtasks /Delete /F /TN NInferDraftCtrl 2>$null | Out-Null
schtasks /Delete /F /TN NInferServe 2>$null | Out-Null
Stop-Process -Name ninfer-serve -Force -ErrorAction SilentlyContinue
Remove-Item "$env:USERPROFILE\Desktop\NInfer Console.lnk" -Force -ErrorAction SilentlyContinue
Write-Host 'Uninstalled (tasks + shortcut removed; logs/model/cache kept).'
