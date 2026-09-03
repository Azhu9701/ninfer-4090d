@echo off
REM NInfer read-only console launcher: close window anytime, service unaffected
start "NInfer Console" powershell -ExecutionPolicy Bypass -NoExit -File "%~dp0ninfer_console.ps1"
