@echo off
REM start NInfer service + draft controller
setlocal
cd /d "%~dp0"
call config\settings.cmd
schtasks /Run /TN NInferServe
schtasks /Run /TN NInferDraftCtrl
echo Waiting for health on port %NINFER_PORT% ...
:wait
timeout /t 5 >nul
curl -s -m 3 http://127.0.0.1:%NINFER_PORT%/health 2>nul | findstr /C:"ok" >nul && ( echo HEALTHY & goto :eof )
goto :wait
