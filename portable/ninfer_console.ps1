param(
    [string]$Root = (Split-Path -Parent $MyInvocation.MyCommand.Path),
    [switch]$Once
)
# NInfer read-only console: tails task_err / task_out / controller.log
# Principle: log files are the single source of truth; this is a read-only
# viewer (FileShare.ReadWrite) - closing the window never affects the service.
$ErrorActionPreference = 'Continue'

$targets = @(
    @{ Tag = 'SRV'; Color = 'Red';  Path = "$Root\logs\task_err.log" },
    @{ Tag = 'OUT'; Color = 'Gray'; Path = "$Root\logs\task_out.log" },
    @{ Tag = 'CTL'; Color = 'Cyan'; Path = "$Root\logs\controller.log" }
)

function Open-TailReader([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    $fs = [System.IO.FileStream]::new($path, 'Open', 'Read', 'ReadWrite')
    $start = [Math]::Max(0, $fs.Length - 8192)   # initial context: last 8KB of each file
    $sr = [System.IO.StreamReader]::new($fs, [System.Text.Encoding]::UTF8)
    $fs.Position = $start
    if ($start -gt 0) { $null = $sr.ReadLine() }  # drop partial line
    return @{ SR = $sr; FS = $fs; Offset = $fs.Position }
}

function Write-Header {
    $mem   = (& nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader 2>$null)
    $draft = '?'
    if (Test-Path "$Root\state\current_draft.txt") { $draft = (Get-Content "$Root\state\current_draft.txt" -ErrorAction SilentlyContinue) }
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
            # rotated/truncated: reopen
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
Write-Host 'NInfer read-only console (Ctrl+C or close window to exit; service unaffected)' -ForegroundColor Yellow
foreach ($t in $targets) { $t.Reader = Open-TailReader $t.Path; Read-Target $t }

$tick = 0
while ($true) {
    Start-Sleep -Seconds 2
    foreach ($t in $targets) {
        if ($null -eq $t.Reader) { $t.Reader = Open-TailReader $t.Path }
        Read-Target $t
    }
    $tick++
    if ($tick -ge 8) { $tick = 0; Write-Header }   # status header ~every 16s
    if ($Once) { break }
}
