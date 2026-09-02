# NInfer-4090D — Qwen3.8-27B 生产部署实录（RTX 4090 D 48GB）

> 在 **UDPSendToFailed/ninfer-4090**（RTX 4090 24GB）基础上，将 NInfer 推理引擎完整落地到 **RTX 4090 D（48GB 中国特供版）** 的生产配置、踩坑记录与调优数据。
>
> 引擎/模型未做任何代码修改——本仓库的价值在于：**4090 D 的实证配置、静默崩溃排查方法论、以及一份可直接复制的 Windows 计划任务部署方案**。

[![GPU](https://img.shields.io/badge/GPU-RTX%204090%20D%2048GB-76b900)](https://www.nvidia.cn/geforce/) [![Model](https://img.shields.io/badge/Model-Qwen3.8--27B-6c5ce7)](https://huggingface.co/Qwen) [![Upstream](https://img.shields.io/badge/upstream-UDPSendToFailed%2Fninfer--4090-blue)](https://github.com/UDPSendToFailed/ninfer-4090)

---

## 1. 硬件平台（实测 Ground Truth）

| 部件 | 规格 | 备注 |
|---|---|---|
| **GPU** | NVIDIA GeForce RTX 4090 D | **48 GB GDDR6X**（49140 MiB），AD102 裁剪版 |
| SM 数 | **114**（非 4090 的 128） | 已用 `NINFER_TARGET_SM_COUNT=114` 构建对齐 |
| CPU | AMD EPYC 7K62，48 核 96 线程 | AVX2 |
| 内存 | **512 GB** DDR4 | 兜磁盘缓存的页缓存 |
| 系统/NVMe | Windows + ORICO 4TB NVMe | 磁盘缓存所在盘（DirectStorage 要求 NVMe） |
| 驱动 / CUDA | 616.56 / 13.3 | |
| 网络 | Tailscale 异地组网 | 服务 bind 0.0.0.0，经 Tailscale IP 远程调用 |

4090 D 与原版 4090 的两个本质差异（沿用上游文档结论，实测吻合）：

1. **SM 128 → 114**：所有按 128 SM 定波的内核网格在 4090 D 上产生残波。上游 `feat/rtx-4090-sm89-native` 分支的 `NINFER_TARGET_SM_COUNT` 编译期宏是正解——本项目构建时传 `-DNINFER_TARGET_SM_COUNT=114`，GQA INT8 decode 网格落在 112 CTA = **0.98 波**（完美单波）。
2. **VRAM 24 → 48 GB**：KV 池直接翻倍起步，128K 上下文 + 4 路并发下仍剩 ~19 GB 余量。

## 2. 实测性能（生产配置，Tailscale 远程实测）

引擎 `ninfer-serve.exe`，模型 `qwen3_8_27b.ninfer`（16.67 GiB groupwise-int）：

| 测试项 | 配置 | 吞吐 | 备注 |
|---|---|---:|---|
| **代码续写（greedy）** | MTP7 + `temperature=0` | **195–203 tok/s** | MTP 接受率最高的场景 |
| 短 prompt 解码 | MTP7，英文 essay | 77–101 tok/s | 冷内容随机生成，MTP 难预测 |
| 中文对话 | MTP7 | 70–80 tok/s | 同上 |
| **64K prefill** | 64,491 tokens | **1,667–1,671 tok/s** | 38.8s 全量重算 |
| **64K 磁盘缓存命中** | 同 payload 第 2 次 | **1.9s TTFT** | NVMe→VRAM DMA，**20x 提速** |
| 64K 命中（跨进程重启） | 重启服务后同 payload | **1.8s** | 缓存持久化，10+ GB/s 恢复 |
| 4 路并发（静态 draft=7） | `--max-concurrency 4` | ~178 tok/s 总计 | 长 draft 的验证浪费 |
| **4 路并发（动态 draft=2）** | `draft_controller.ps1` | **~264 tok/s 总计** | **+48%，见踩坑 #6** |
| 权重加载 | 冷启动 | 16.67 GiB / 4.9s | pinned 双缓冲 DMA |
| 模型总加载 | 到 listening | **6.5–7.8s** | 含 KV 池分配与 warmup |
| MTP 接受率 | draft=7，通用对话 | ~40%（6.7 tok/round） | 代码类显著更高 |

对比上游 24 GB 4090 公布数据（同模型）：pp2048 prefill 2,093 tok/s、MTP7 @2k 218 tok/s、基线 decode 52.8 tok/s——4090 D 在同等构建下表现一致，且 KV 容量翻倍。

## 3. 生产配置（可直接复制）

`serve_task.bat`（Windows 计划任务引用，开机自启 + 崩溃由任务计划重启）：

```bat
@echo off
cd /d C:\Users\Administrator\Documents\ninfer\udf\build-ninja\apps
ninfer-serve.exe C:\models\qwen3_8_27b.ninfer ^
  --host 0.0.0.0 --port 8080 ^
  --max-context 131072 ^
  --kv-dtype rk4v4-e8 --kv-capacity auto ^
  --max-concurrency 4 --max-pending-requests 64 ^
  --api-key sk-your-key-here ^
  --spec mtp --draft-tokens 7 --lm-head-draft ^
  --preserve-thinking ^
  --disk-cache --disk-cache-dir C:\ninfer-cache --disk-cache-gb 200 ^
  --request-log-jsonl C:\ninfer\logs\requests.jsonl
```

注册为计划任务（脱离 SSH 会话，登录自启，最高权限）：

```powershell
schtasks /Create /TN NInferServe /TR C:\ninfer\serve_task.bat /SC ONLOGON /RL HIGHEST /F
schtasks /Run /TN NInferServe
```

### 参数逐条依据

| 参数 | 值 | 为什么 |
|---|---|---|
| `--kv-dtype` | `rk4v4-e8` | 4-bit E8 晶格，98.7% KV 保真（上游矩阵）。48GB 下 524K tokens 池仍显存充裕；编码/数学场景比 2-bit 的 `rk2v4-e8`（96.2%）更稳 |
| `--max-context` | `131072` | 128K 覆盖绝大多数 agent 任务；KV 上限 = ctx × concurrency（见踩坑 #3） |
| `--max-concurrency` | `4` | 4 slot 吃满 KV 上限到 524K tokens；单路延迟几乎无损 |
| `--spec mtp --draft-tokens 7` | MTP7 | **serve 模式实测天花板**。8/9 触发 `cudaErrorGraphExecUpdateFailure`（CUDA Graph 对 W>7 验证宽度更新失败，上游 bench 模式可到 MTP7+，serve 不行）。代码 greedy 场景 4→7 提速 32% |
| `--lm-head-draft` | 开 | draft 走专用 proposal head，draft 词表投影更小 |
| `--disk-cache` + `200GB` | C:\ninfer-cache | **必须放 NVMe**（DirectStorage 1.3 DMA 路径）。重复前缀 TTFT 从 38.8s → 1.9s，重启不丢 |
| `--preserve-thinking` | 开 | 上游 README 推荐对话姿势 |
| `--api-key` | 必设 | 服务 bind 0.0.0.0 供 Tailscale 远程调用时防止网内裸奔 |
| ❌ `--wddm-evictable-budget` | **禁用** | 见踩坑 #1，这是本部署最重要的负优化 |

## 4. 踩坑记录（4090 D 特有，每条都付了学费）

### 坑 #1：`--wddm-evictable-budget` + 桌面驱动卡 = 静默崩溃

上游该 flag 的语义：按**总显存**做预算，允许 WDDM 把后台应用显存驱逐到 512 MiB DWM 底线（`src/targets/registry.cpp` 的 `kMinDwmHeadroom`）。上游文档明确警告："on a card that is also driving the desktop... oversubscribes the device and pushes the runtime into WDDM paging"。

我们的 4090 D **正在驱动桌面**（DWM + 一堆桌面 App），实测两种死法：

- 启动即死：加载权重阶段过度订阅，进程无声退出（stderr 只有 "loading model..."）
- 运行中死：8K prefill 进行到 1,113 tokens 时静默消失，无任何异常日志

**定位方法**（三组二分，每组 45s 存活 + 真实 64K 请求验证）：`A=无MTP无wddm / B=有MTP无wddm / C=有MTP有wddm`，结论——MTP 无罪、wddm 在本卡必死。**驱动桌面的卡一律不要开这个 flag**；专职推理卡（无显示器）才可尝试。

### 坑 #2：sshd 拉起的进程随 SSH 会话一起死

三个不同配置的实例都呈现"运行 1–3 分钟后静默退出"，且 stdout/stderr 无任何错误、Windows 事件日志无 crash 记录。最终对照实验定位：**同一命令，前台交互 SSH 会话里跑活着，`ssh host command` 非交互方式跑必死**——SSH 会话关闭时的作业清理连带杀掉子进程。

**解法**：Windows 计划任务（`schtasks /SC ONLOGON /RL HIGHEST`）。它有独立会话、开机自启、崩溃自动重启三重保障。任何 Windows GPU 服务器的生产部署都应该用这个方式，而不是 sshd 拉起。

### 坑 #3：KV 池上限 = max-context × max-concurrency（隐式约束）

源码 `src/targets/qwen3_6/impl/runtime/layouts_impl.h`：

```cpp
const uint32_t logical_pages = page_count(options.max_context);
const uint64_t maximum_pages64 = (uint64_t)options.max_concurrency * logical_pages;
```

我们最初 22.9 GB 显存一直闲置——`131072 × 2 = 262K` tokens 封顶，`--kv-capacity auto` 撑死也只有这个数。把并发提到 4 后自动扩到 **524,288 tokens（11.68 GiB）**。想再扩：加并发（1–8）或加 ctx，两者乘积才是真上限。

### 坑 #4：MTP draft=8/9 在 serve 模式崩溃

CLI 帮助写 `--draft-tokens 1..15`，但 serve 模式下 8 和 9 都触发 `cudaErrorGraphExecUpdateFailure (update result 2)` 进程退出。CUDA Graph 捕获的验证内核宽度上限在 serve 路径是 7（与上游 bench 表格里 serve 场景最高只列 MTP7 吻合）。**7 就是甜点，别贪**。

### 坑 #5：客户端代理截胡 Tailscale 流量

Mac 客户端挂着 Clash 时，`http://100.77.174.21:8080` 会被代理劫持返回 502——**表现为"服务挂了"，实际服务活得好好的**。所有客户端进程需要 `export NO_PROXY=100.77.174.21`，curl 加 `--noproxy '*'`。排障时先 `nc -z 100.77.174.21 8080` 确认端口再怀疑服务。

### 坑 #6：MTP draft 深度的最优值随并发剧烈变化（D-cut 效应实证）

上游/D-cut (arXiv 2026) 的结论在 4090 D 上完全复现，而且比论文更陡峭。同一 4 路并发负载（长 prompt、temperature=0、3 轮取均值）扫 draft 深度：

| draft-tokens | 4 路并发总吞吐 | 单路（对照） |
|---:|---:|---:|
| **2** | **264 tok/s** ← 并发最优 | ~85 |
| 3 | 254 tok/s | — |
| 1 | 233 tok/s | — |
| 4 | 186 tok/s | ~95 |
| 7 | 178 tok/s | **195–203** ← 单路最优 |

机理解读：并发 decode 时 GPU 已被多路分摊，MTP 验证是"一次 forward 验证 W 个 draft token"——draft 越长，**被拒绝 token 的验证算力浪费越大**，且 draft head 的 KV 读取也随长度线性涨（Windowed-MTP, arXiv 2026 指出该读取随全上下文增长）。低并发时 GPU 空闲算力充裕，长 draft 的收益（更多并行接受）占上风。

**解法**：`draft_controller.ps1`（本仓库）——计划任务常驻的控制器，10s 采样 `/metrics` 的 `requests_processing`，连续 3 次 ≥2 并发切 `draft=2`、连续 6 次 ≤1 并发切回 `draft=7`（低档回切防抖更长，避免打断进行中的请求）。切换 = 改 bat + 计划任务重启 serve（加载 6.5s）。已在生产运行，双档自动切换实测无感。

## 5. 部署检查清单

```text
[ ] 构建时 -DNINFER_TARGET_SM_COUNT=114（验证: build.ninja 里 DEFINES 含 NINFER_TARGET_SM_COUNT=114）
[ ] nvidia-smi 空闲显存 > 22 GB（桌面应用是显存刺客）
[ ] 不要开 --wddm-evictable-budget（除非卡不接显示器）
[ ] 用 schtasks 部署，不要用 sshd 拉起
[ ] 磁盘缓存目录放 NVMe 盘
[ ] 防火墙放行 8080 仅限 Tailscale 网段（100.64.0.0/10）
[ ] --api-key 必设
[ ] 客户端 NO_PROXY 设置好
```

## 6. Agent 服务化（当前生产状态）

本机最终形态：OpenAI/Anthropic 双协议兼容的常驻推理服务，经 Tailscale 供异地 agent 调用。

```
Base URL : http://<tailscale-ip>:8080/v1        (OpenAI 兼容)
           http://<tailscale-ip>:8080/v1/messages (Anthropic 兼容)
Model    : qwen3.8-27b
Auth     : Authorization: Bearer <api-key>
```

- 代码任务 `temperature=0`（MTP 接受率最大化，~195 tok/s）
- 长文档多轮问答吃磁盘缓存红利（重复前缀 TTFT ~2s）
- 并发 ≤4 路最佳，更多自动排队（`--max-pending-requests 64`）

## 7. 与上游的差异清单

相对 [UDPSendToFailed/ninfer-4090](https://github.com/UDPSendToFailed/ninfer-4090)@`3d11fd64`（2026-09-01），本项目**零代码差异**（`src/` 全量 diff 一致，仅 `core/device.h` 的 `NINFER_TARGET_SM_COUNT` 宏化改动来自上游自身的 4090 D 支持），差异全部在**部署与运行配置层**：

1. `NINFER_TARGET_SM_COUNT=114` 构建参数（上游 CMake 已原生支持，传入即可）
2. 生产 serve 参数组（MTP7 + rk4v4-e8 + no-wddm，见第 3 节）
3. Windows 计划任务部署框架（脱离 SSH 会话）
4. 4090 D 真机的静默崩溃排查结论与检查清单（第 4 节）

## 8. 致谢与引用

本工作完全站在前人肩膀上，感谢以下仓库与项目：

- **[Neroued/ninfer](https://github.com/Neroued/ninfer)** — NInfer 引擎本体（fork 链源头）
- **[Don-Chad/ninfer-3090](https://github.com/Don-Chad/ninfer-3090)** — RTX 3090 (sm_86) 移植，fork 链中间层
- **[sergiuszm/ninfer-4090](https://github.com/sergiuszm/ninfer-4090)** — RTX 4090 (sm_89) 移植，E8 晶格 KV 量化系统，262K 上下文验证
- **[UDPSendToFailed/ninfer-4090](https://github.com/UDPSendToFailed/ninfer-4090)** — 本项目的直接基底：`NINFER_TARGET_SM_COUNT` 宏化（使 4090 D 的 114 SM 对齐成为一行编译参数）、GQA decode 单波对齐、DirectStorage 1.3 磁盘缓存、MTP 容量扩展、全部 kernel 级优化
- **[xkeyC/ninfer-4090](https://github.com/xkeyC/ninfer-4090)** — 同源 fork，其 README 数据用于交叉验证
- **[Qwen](https://huggingface.co/Qwen)** — Qwen3.8-27B 模型
- 引擎架构与 E8 (Conway–Sloane) 晶格量化思想源自 NInfer 上游系列仓库的集体工作

## 9. 免责声明

个人硬件实验记录，按"现状"提供。驱动版本、Windows 版本、后台应用组合都会影响结果——尤其是踩坑 #1（WDDM）和 #2（SSH 会话），在别的机器上表现可能不同。上游免责声明同样适用。
