@echo off
REM stop NInfer service + draft controller
setlocal
cd /d "%~dp0"
schtasks /End /TN NInferDraftCtrl 2>nul
schtasks /End /TN NInferServe 2>nul
timeout /t 2 >nul
taskkill /F /IM ninfer-serve.exe 2>nul
echo stopped
