param([string]$apiKey = "sk-ninfer-6b30ca0a0bf799b73a545c61c9e45e26")
$ErrorActionPreference = 'Continue'
$bat = 'C:\Users\Administrator\Documents\ninfer\serve_task.bat'
$log = 'C:\Users\Administrator\Documents\ninfer\logs\controller.log'
$stateFile = 'C:\Users\Administrator\Documents\ninfer\current_draft.txt'

function Log($m) { $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'; Add-Content -Path $log -Value "[$ts] $m" }

function Get-Load {
    try {
        $resp = Invoke-WebRequest -Uri 'http://127.0.0.1:8080/metrics' -Headers @{ Authorization = "Bearer $apiKey" } -TimeoutSec 5 -UseBasicParsing
        if ($resp.Content -match 'llamacpp:requests_processing (\d+)') { return [int]$Matches[1] }
    } catch { Log ("metrics err: " + $_.Exception.Message.Substring(0, [Math]::Min(80, $_.Exception.Message.Length))) }
    return -1
}

function Switch-Draft([int]$newDraft) {
    Log "SWITCH draft->$newDraft (restarting serve)"
    (Get-Content $bat -Raw) -replace '--draft-tokens \d+', "--draft-tokens $newDraft" | Set-Content $bat -Encoding ASCII
    Set-Content -Path $stateFile -Value $newDraft
    Stop-Process -Name ninfer-serve -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 4
    schtasks /Run /TN NInferServe | Out-Null
    # 等监听
    for ($i=0; $i -lt 30; $i++) { Start-Sleep -Seconds 3; if ((netstat -ano | Select-String ':8080.*LISTENING')) { break } }
    Log "SWITCH done, serve listening (draft=$newDraft)"
}

# 主循环
Log "controller started"
while ($true) {
    Log ("tick load=" + (Get-Load))
    Start-Sleep -Seconds 10
    $load = Get-Load
    if ($load -lt 0) { continue }   # 服务没起或查询失败, 不动作
    $cur = 7
    if (Test-Path $stateFile) { $cur = [int](Get-Content $stateFile -ErrorAction SilentlyContinue) }

    # 防抖计数器: 连续N次采样同一档位才切
    if ($load -ge 2 -and $cur -ne 2) {
        $hiStreak++
        $loStreak = 0
        if ($hiStreak -ge 3) { Switch-Draft 2; $hiStreak = 0 }
    } elseif ($load -le 1 -and $cur -ne 7) {
        $loStreak++
        $hiStreak = 0
        if ($loStreak -ge 6) { Switch-Draft 7; $loStreak = 0 }
    } else {
        $hiStreak = 0; $loStreak = 0
    }
}
