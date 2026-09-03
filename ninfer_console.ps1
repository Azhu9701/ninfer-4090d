param(
    [string]$Base = 'C:\Users\Administrator\Documents\ninfer',
    [switch]$Once
)
# NInfer 只读控制台：追踪 task_err / task_out / controller.log
# 原则：日志文件是唯一真相，本脚本只是只读观众（FileShare.ReadWrite），
#       关窗口 / Ctrl+C 任何时候都安全，绝不影响 serve 进程。
$ErrorActionPreference = 'Continue'

$targets = @(
    @{ Tag = 'SRV'; Color = 'Red';  Path = "$Base\logs\task_err.log" },
    @{ Tag = 'OUT'; Color = 'Gray'; Path = "$Base\logs\task_out.log" },
    @{ Tag = 'CTL'; Color = 'Cyan'; Path = "$Base\logs\controller.log" }
)

function Open-TailReader([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    $fs = [System.IO.FileStream]::new($path, 'Open', 'Read', 'ReadWrite')
    $start = [Math]::Max(0, $fs.Length - 8192)   # 初始上下文：每个文件最后 8KB
    $sr = [System.IO.StreamReader]::new($fs, [System.Text.Encoding]::UTF8)
    $fs.Position = $start
    if ($start -gt 0) { $null = $sr.ReadLine() }  # 丢弃半行
    return @{ SR = $sr; FS = $fs; Offset = $fs.Position }
}

function Write-Header {
    $mem   = (& nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader 2>$null)
    $draft = '?'
    if (Test-Path "$Base\current_draft.txt") { $draft = (Get-Content "$Base\current_draft.txt" -ErrorAction SilentlyContinue) }
    $state = if (Get-Process ninfer-serve -ErrorAction SilentlyContinue) { 'RUNNING' } else { 'DOWN' }
    Write-Host ''
    Write-Host ("== ninfer-serve={0}  draft={1}  gpu={2}  {3} ==" -f $state, $draft, $mem, (Get-Date -Format 'HH:mm:ss')) -ForegroundColor Yellow
    Write-Host ''
}

function Read-Target($t) {
    $r = $t.Reader
    if ($null -eq $r) { return }
    try {
        $len = $r.FS.Length
        if ($len -lt $r.Offset) {
            # 文件被轮转/清空：重开
            $r.SR.Dispose(); $r.FS.Dispose()
            $t.Reader = Open-TailReader $t.Path
            return
        }
        while ($true) {
            $line = $r.SR.ReadLine()
            if ($null -eq $line) { break }
            $r.Offset = $r.FS.Position
            $ts = Get-Date -Format 'HH:mm:ss'
            Write-Host "[$ts $($t.Tag)] " -NoNewline -ForegroundColor $t.Color
            Write-Host $line
        }
    } catch {
        $t.Reader = $null
        try { $r.SR.Dispose(); $r.FS.Dispose() } catch {}
    }
}

Clear-Host
Write-Host 'NInfer 只读控制台 (Ctrl+C 或关窗退出，不影响服务)' -ForegroundColor Yellow
foreach ($t in $targets) { $t.Reader = Open-TailReader $t.Path; Read-Target $t }

$tick = 0
while ($true) {
    Start-Sleep -Seconds 2
    foreach ($t in $targets) {
        if ($null -eq $t.Reader) { $t.Reader = Open-TailReader $t.Path }
        Read-Target $t
    }
    $tick++
    if ($tick -ge 8) { $tick = 0; Write-Header }   # ~16s 刷一次状态头
    if ($Once) { break }
}
