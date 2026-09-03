@echo off
REM NInfer portable package - central configuration (single source of truth)
REM Consumed by serve.bat (cmd) and *.ps1 scripts (parsed as set "K=V" lines)

set "NINFER_HOST=0.0.0.0"
set "NINFER_PORT=8080"
set "NINFER_MODEL=models\qwen3_8_27b.ninfer"

set "NINFER_MAX_CONTEXT=131072"
set "NINFER_KV_DTYPE=rk4v4-e8"
set "NINFER_MAX_CONCURRENCY=4"
set "NINFER_MAX_PENDING=64"

REM CHANGE THIS before exposing beyond localhost:
set "NINFER_API_KEY=sk-CHANGE-ME"

set "NINFER_DRAFT=7"
set "NINFER_VISION=1"
set "NINFER_CACHE_DIR=C:\ninfer-cache"
set "NINFER_CACHE_GB=200"
