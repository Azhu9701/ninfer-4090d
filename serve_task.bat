@echo off
REM =====================================================================
REM  NInfer 4090D 生产启动脚本 (Windows 计划任务引用)
REM  用法:
REM    schtasks /Create /TN NInferServe /TR <本文件路径> /SC ONLOGON /RL HIGHEST /F
REM    schtasks /Run /TN NInferServe
REM  注意:
REM   - 不要用 sshd 非交互会话拉起 (进程会随会话被清, 见 TUNING_LOG.md 坑#2)
REM   - 驱动桌面的卡不要加 --wddm-evictable-budget (坑#1)
REM   - 磁盘缓存目录必须在 NVMe 盘 (DirectStorage 要求)
REM   - --vision 开启视觉输入 (默认关闭省显存; 运行时不可补开, 需重启; 与 DFlash 互斥, 与 MTP 可共存)
REM   - 计划任务动作直接指 powershell -File, 不要 cmd→start /B 多层包装 (坑#3 作业对象收割子进程)
REM =====================================================================

REM ---- 按需修改的路径 ----
set "APPS=C:\ninfer\udf\build-ninja\apps"
set "MODEL=C:\ninfer\models\qwen3_8_27b.ninfer"
set "LOGDIR=C:\ninfer\logs"
set "CACHEDIR=C:\ninfer-cache"
set "APIKEY=sk-change-me"

cd /d "%APPS%"
ninfer-serve.exe "%MODEL%" ^
  --host 0.0.0.0 --port 8080 ^
  --max-context 131072 ^
  --kv-dtype rk4v4-e8 --kv-capacity auto ^
  --max-concurrency 4 --max-pending-requests 64 ^
  --api-key %APIKEY% ^
  --vision ^
  --spec mtp --draft-tokens 7 --lm-head-draft ^
  --preserve-thinking ^
  --disk-cache --disk-cache-dir %CACHEDIR% --disk-cache-gb 200 ^
  --request-log-jsonl %LOGDIR%\requests.jsonl ^
  >> "%LOGDIR%\task_out.log" 2>> "%LOGDIR%\task_err.log"
