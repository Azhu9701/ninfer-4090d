# NInfer 便携部署包（RTX 4090 D · Qwen3.8-27B）

单目录自包含部署：整个文件夹拷到任何 Windows 机器（有 NVIDIA 显卡），改一行配置即可运行。

## 布局

```
ninfer-portable/
  bin/                  ninfer-serve.exe + 依赖 DLL（ffmpeg/dstorage/libcurl）
  models/               qwen3_8_27b.ninfer（16.96 GiB，需单独拷入）
  config/settings.cmd   唯一配置源（端口/密钥/上下文/缓存目录…）
  logs/                 运行日志（自动轮转，保留 2 代 × 100MB）
  state/                控制器状态（current_draft.txt, controller.pid）
  serve.bat             服务入口（计划任务调用）
  draft_controller.ps1  动态草稿控制器（单实例守护）
  install.ps1           安装：注册开机任务 + 桌面日志控制台快捷方式
  uninstall.ps1         卸载：删任务与快捷方式（保留模型/日志/缓存）
  start.bat / stop.bat  手动启停
  status.ps1            状态快照（进程/显存/draft 档位/健康/负载）
  console.bat           只读日志控制台（双击即看，关窗不影响服务）
```

## 三步上线

```bat
:: 1. 把 qwen3_8_27b.ninfer 拷进 models\
:: 2. 编辑 config\settings.cmd（至少改 NINFER_API_KEY、确认 NINFER_CACHE_DIR 指向 NVMe 盘）
:: 3. 安装并启动
powershell -ExecutionPolicy Bypass -File install.ps1
start.bat
```

卸载：`powershell -ExecutionPolicy Bypass -File uninstall.ps1`（不动模型/日志/缓存）。

## 当前生产参数（config\settings.cmd）

| 项 | 值 | 依据 |
|---|---|---|
| max-context | 131072 | 128K 甜点区（240K 会掉速到 ~50 tok/s 且吃满显存） |
| kv-dtype | rk4v4-e8 | 98.7% 保真实测 |
| max-concurrency × pending | 4 × 64 | KV 池 524K tokens 吃满余量 |
| spec mtp draft 7 | 控制器按负载 7↔2 | 单路 195 tok/s；4 路并发时 2 最优（+48%） |
| --vision | 开 | 启动期开关，运行时不可补开 |
| --disk-cache | 200GB @ NVMe | 64K prompt TTFT 38.8s → 1.9s |

## 运维要点

- **不要**给驱动桌面的卡加 `--wddm-evictable-budget`（静默崩溃）
- 计划任务动作直指 powershell，不要 cmd→start 多层包装（作业对象收割子进程）
- 恢复后首次请求慢是正常的（WDDM 驱逐），发一次 64K 大 prefill 焐热即可
- 日志是唯一真相；控制台只是只读观众；控制器以心跳为准，进程活着≠活着

上游与优化细节见仓库根 README.md / TUNING_LOG.md。
