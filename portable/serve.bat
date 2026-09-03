@echo off
REM ============================================================
REM  NInfer portable package - serve task action
REM  All paths relative to package root. Config: config\settings.cmd
REM ============================================================
setlocal
cd /d "%~dp0"
call config\settings.cmd

if not exist "bin\ninfer-serve.exe" ( echo [FATAL] bin\ninfer-serve.exe missing & exit /b 1 )
if not exist "%NINFER_MODEL%" ( echo [FATAL] model missing: %NINFER_MODEL% & exit /b 1 )

REM ---- log rotation: keep last 2 generations, 100MB threshold ----
if exist logs\task_err.log (
  for %%A in (logs\task_err.log) do if %%~zA GTR 104857600 (
    if exist logs\task_err.1.log del /q logs\task_err.1.log
    ren logs\task_err.log task_err.1.log
  )
)
if exist logs\task_out.log (
  for %%A in (logs\task_out.log) do if %%~zA GTR 104857600 (
    if exist logs\task_out.1.log del /q logs\task_out.1.log
    ren logs\task_out.log task_out.1.log
  )
)

set "VISION="
if /I "%NINFER_VISION%"=="1" set "VISION=--vision"

echo [%date% %time%] starting ninfer-serve (port=%NINFER_PORT% draft=%NINFER_DRAFT%) >> logs\serve_start.log

bin\ninfer-serve.exe "%~dp0%NINFER_MODEL%" %VISION% ^
  --host %NINFER_HOST% --port %NINFER_PORT% ^
  --max-context %NINFER_MAX_CONTEXT% ^
  --kv-dtype %NINFER_KV_DTYPE% --kv-capacity auto ^
  --max-concurrency %NINFER_MAX_CONCURRENCY% --max-pending-requests %NINFER_MAX_PENDING% ^
  --api-key %NINFER_API_KEY% ^
  --spec mtp --draft-tokens %NINFER_DRAFT% --lm-head-draft ^
  --preserve-thinking ^
  --disk-cache --disk-cache-dir %NINFER_CACHE_DIR% --disk-cache-gb %NINFER_CACHE_GB% ^
  --request-log-jsonl "%~dp0logs\requests.jsonl" ^
  >> "%~dp0logs\task_out.log" 2>> "%~dp0logs\task_err.log"
