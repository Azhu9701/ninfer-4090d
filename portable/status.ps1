# NInfer portable package - one-shot status snapshot
param([string]$Root = (Split-Path -Parent $MyInvocation.MyCommand.Path))
$ErrorActionPreference = 'Continue'

$cfg = @{}
Get-Content "$Root\config\settings.cmd" | ForEach-Object {
    if ($_ -match '^set "(\w+)=(.*)"$') { $cfg[$Matches[1]] = $Matches[2] }
}
$port = $cfg['NINFER_PORT']; $key = $cfg['NINFER_API_KEY']

$proc = Get-Process ninfer-serve -ErrorAction SilentlyContinue
Write-Host ("service : " + $(if ($proc) { "RUNNING (pid $($proc.Id), $([math]::Round($proc.WorkingSet64/1GB,1)) GB RSS)" } else { 'DOWN' }))
Write-Host ("gpu     : " + (& nvidia-smi --query-gpu=memory.used,memory.total,utilization.gpu --format=csv,noheader 2>$null))
$draft = '?'; if (Test-Path "$Root\state\current_draft.txt") { $draft = Get-Content "$Root\state\current_draft.txt" }
Write-Host ("draft   : $draft  (config default: $($cfg['NINFER_DRAFT']))")

try {
    $h = Invoke-WebRequest -Uri "http://127.0.0.1:$port/health" -TimeoutSec 3 -UseBasicParsing
    Write-Host ("health  : " + $h.StatusCode + ' ' + $h.Content)
} catch { Write-Host 'health  : UNREACHABLE' }

try {
    $m = (Invoke-WebRequest -Uri "http://127.0.0.1:$port/metrics" -Headers @{ Authorization = "Bearer $key" } -TimeoutSec 3 -UseBasicParsing).Content
    if ($m -match 'llamacpp:requests_processing (\d+)') { Write-Host ("load    : " + $Matches[1] + ' processing') }
    if ($m -match 'llamacpp:prompt_tokens_total (\d+)') { Write-Host ("tokens  : prompt=" + $Matches[1]) }
} catch { Write-Host 'metrics : UNREACHABLE' }

$ctrl = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'draft_controller' }
Write-Host ("ctrl    : " + $(if ($ctrl) { "RUNNING (pid $($ctrl.ProcessId -join ','))" } else { 'DOWN' }))
