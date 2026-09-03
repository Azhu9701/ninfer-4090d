# NInfer portable package - dynamic draft controller
# Single-instance guarded, config-driven, heartbeat-logged.
param([string]$Root = (Split-Path -Parent $MyInvocation.MyCommand.Path))
$ErrorActionPreference = 'Continue'

# ---------- single instance guard ----------
$pidFile = "$Root\state\controller.pid"
if (Test-Path $pidFile) {
    $oldPid = 0
    [void][int]::TryParse((Get-Content $pidFile -ErrorAction SilentlyContinue), [ref]$oldPid)
    if ($oldPid -and (Get-Process -Id $oldPid -ErrorAction SilentlyContinue)) { exit 0 }
}
$PID | Out-File -FilePath $pidFile -Encoding ascii

# ---------- config ----------
$cfg = @{}
Get-Content "$Root\config\settings.cmd" | ForEach-Object {
    if ($_ -match '^set "(\w+)=(.*)"$') { $cfg[$Matches[1]] = $Matches[2] }
}
$port   = $cfg['NINFER_PORT']
$apiKey = $cfg['NINFER_API_KEY']
$log    = "$Root\logs\controller.log"
$state  = "$Root\state\current_draft.txt"

function Log($m) { $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'; Add-Content -Path $log -Value "[$ts] $m" }

function Get-Load {
    try {
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:$port/metrics" -Headers @{ Authorization = "Bearer $apiKey" } -TimeoutSec 5 -UseBasicParsing
        if ($r.Content -match 'llamacpp:requests_processing (\d+)') { return [int]$Matches[1] }
    } catch { Log ("metrics err: " + $_.Exception.Message.Substring(0, [Math]::Min(80, $_.Exception.Message.Length))) }
    return -1
}

function Switch-Draft([int]$newDraft) {
    Log "SWITCH draft->$newDraft (restarting serve)"
    (Get-Content "$Root\config\settings.cmd" -Raw) -replace '(?<=NINFER_DRAFT=)\d+', "$newDraft" |
        Set-Content "$Root\config\settings.cmd" -Encoding ASCII
    Set-Content -Path $state -Value $newDraft
    Stop-Process -Name ninfer-serve -Force -ErrorAction SilentlyContinue
    # wait for GPU memory to actually drain (restarting too early hangs the new instance)
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 3
        $memLine = (nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits) 2>$null
        if ($memLine -match '\d+') { if ([int]$Matches[0] -lt 5000) { break } }
    }
    schtasks /Run /TN NInferServe | Out-Null
    # wait for listener + health
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Seconds 3
        if ((netstat -ano | Select-String ":$port.*LISTENING")) {
            try {
                $h = Invoke-WebRequest -Uri "http://127.0.0.1:$port/health" -TimeoutSec 5 -UseBasicParsing
                if ($h.StatusCode -eq 200) { break }
            } catch {}
        }
    }
    Log "SWITCH done, serve listening (draft=$newDraft)"
}

Log "controller started (pid=$PID port=$port)"
while ($true) {
    Log ("tick load=" + (Get-Load))
    Start-Sleep -Seconds 10
    $load = Get-Load
    if ($load -lt 0) { continue }   # serve down or metrics unreachable: do nothing
    $cur = 7
    if (Test-Path $state) { $cur = [int](Get-Content $state -ErrorAction SilentlyContinue) }

    # debounce: N consecutive same-direction samples before switching
    if ($load -ge 2 -and $cur -ne 2) {
        $hiStreak++; $loStreak = 0
        if ($hiStreak -ge 3) { Switch-Draft 2; $hiStreak = 0 }
    } elseif ($load -le 1 -and $cur -ne 7) {
        $loStreak++; $hiStreak = 0
        if ($loStreak -ge 6) { Switch-Draft 7; $loStreak = 0 }
    } else {
        $hiStreak = 0; $loStreak = 0
    }
}
