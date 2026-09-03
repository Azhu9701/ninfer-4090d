# 4090D 调优复盘（时间线）

> 本文档是 [README](README.md) 的幕后记录：每一步尝试、数据、和当时不理解的东⻄后来怎么被解释的。

## 起点

继承到的部署（`run_4090d_serve.bat`，前人写好但从未稳定跑起来）：

```
ninfer-serve.exe qwen3_8_27b.ninfer
  --max-context 131072 --kv-dtype rk2v4-e8 --kv-capacity auto
  --max-concurrency 2 --spec mtp --draft-tokens 4 --lm-head-draft
  --wddm-evictable-budget        ← 后来的主犯
```

## 时间线

### Phase 0 — 连接与验证
- UU远程 CLI 只能开终端窗口，无法注入输入 → 转向 Tailscale SSH（100.77.174.21）
- `nvidia-smi`: RTX 4090 D, 49140 MiB, driver 616.56 ✓
- 冒烟测试（`ninfer.exe --prompt`）: 15.92 GiB 权重 5.2s 加载, decode 49 tok/s → 引擎与模型本身健康

### Phase 1 — 复现"静默死亡"
- 后台 Start-Process 起 serve：stderr 停在 `loading model...`，进程消失，stdout 空，事件日志干净
- 三次复现，两个不同死点：启动时死；一次跑到 8K prefill 中间（日志止于 prefill 1,113 tokens）死

### Phase 2 — 二分定位
写了个三组二分脚本（45s 存活窗口 + 真实 64K 请求验证）：

| 组 | flags | 结果 |
|---|---|---|
| A | 无 MTP 无 wddm | ✅ 存活，64K 通过 |
| B | +MTP | ✅ 存活，64K 通过（且 decode 93 vs 42 tok/s） |
| C | +MTP+wddm | ✅ 45s 内活着 —— 但后来补测真实请求时死 |

结论修订：**MTP 无罪，`--wddm-evictable-budget` 在"驱动桌面的卡"上不稳定**。读 `registry.cpp` 找到根因：该 flag 按*总显存*做预算，把桌面应用显存往 512MiB 底线驱逐，在这张正在跑 DWM 的卡上就是过度订阅。

### Phase 3 — "SSH 会话杀进程"真相
- 带 `-Wait` 的前台跑 180s 不退 → 服务其实活着
- 从 Mac 直连 Tailscale IP 测试全 502 → **Clash 代理截胡**（`NO_PROXY` 解围）
- 复盘"三次静默死亡"：全都是 sshd 非交互会话拉起的进程随会话被清。前台交互会话里的同命令活得好好的
- **修法**：`schtasks /Create /TN NInferServe /SC ONLOGON /RL HIGHEST` —— 独立会话 + 开机自启 + 崩溃重启

### Phase 4 — 速度调优
- A/B `temperature 1.0 vs 0`：代码类 greedy 下 MTP 接受率显著更高
- `--draft-tokens 4→7`：代码续写 149 → **197 tok/s**
- 试 8、9：`cudaErrorGraphExecUpdateFailure` → serve 模式 MTP 天花板就是 7（与上游 bench 数据只在 serve 列 MTP7 吻合）
- 中文/创意文本 MTP 收益小（接受率 ~40%）：符合物理，draft 难预测随机内容

### Phase 5 — 磁盘缓存
- `--disk-cache` 放 C 盘（ORICO NVMe；D 盘 Samsung 860 是 SATA，DirectStorage 不适用）
- 64K payload：38.8s → **1.9s**（第二次），**1.8s**（服务重启后）→ NVMe→VRAM DMA 恢复 ~10GB/s，与上游 150ms/1.5GiB 数据同量级

### Phase 6 — 吃满显存
- 发现 22.9GB 显存闲置 → 读 `layouts_impl.h` 找到 `maximum_pages = max_concurrency × page_count(max_context)`
- concurrency 2→4：KV 池 262K → **524,288 tokens**（11.68 GiB），KV 池自动翻倍
- 4 路并发实测：每路 ~40 tok/s，总 ~159 —— 27B dense 的 decode 是带宽瓶颈，并发不加总量，但多 slot 对多 agent 场景有价值

### Phase 7 — 服务化
- `--api-key` 上鉴权（bind 0.0.0.0 暴露给 Tailscale 网段）
- 三协议端到端验证：OpenAI chat/completions ✓、SSE 流式 ✓、Anthropic messages ✓

## 最终配置 vs 初始配置

| 项 | 初始 | 最终 | 变化原因 |
|---|---|---|---|
| wddm-evictable-budget | 开 | **删** | 坑#1 静默崩溃 |
| draft-tokens | 4 | **7** | +32% 代码解码 |
| kv-dtype | rk2v4-e8 | **rk4v4-e8** | 98.7% 保真，48G 撑得起 |
| max-concurrency | 2 | **4** | KV 上限翻倍 524K |
| max-pending | 32 | **64** | agent 突发 |
| disk-cache | 无 | **200GB @NVMe** | TTFT 20x |
| 部署方式 | 手工/SSH | **schtasks** | 坑#2 |
| api-key | 无 | **有** | 暴露面治理 |
| 代码速度 | — | **195 tok/s** | — |

### Phase 8 — 视觉与控制器任务修复 (2026-09-03)
- `--vision` 是启动期开关：**运行时不可补开**（上游文档明言 "A later request cannot enable a capability omitted at startup"）。默认关闭省显存；中转站报 `Vision is disabled for this server` 时先查服务启动参数，不是模型不支持
- 与 `--spec dflash` 互斥，与 `--spec mtp` 可共存（本配置 vision+MTP7 实测正常）
- 显存代价：权重后 free 29.5 GiB → 19.4 GiB（视觉塔 + scratch ~10GiB），KV 池 auto 不受影响
- 视觉请求验证：base64 data-URL 红色测试图 → 正确回答「红色」，文本回归 200
- **坑#3：计划任务 `cmd → start /B powershell` 两层包装会被作业对象收割**——cmd 立即退出时任务结束，Task Scheduler 把刚拉起的 powershell 一起杀（时序竞态，之前能活是运气）。修法：任务动作直接指 `powershell -File`，或运行时用 `Start-Process` 拉起脱离父进程
- 排障教训：sshd 管道里的 stderr 会串扰（上次失败命令的「拒绝访问」混进本次结果）；判断进程死活的唯一可信标准是**业务日志有无新心跳**，任务状态码/进程快照都可能骗人

### Phase 9 — 工具调用格式漂移挽救补丁 (2026-09-04)
- **现象**：agent（zcode/pi）长会话中模型偶发输出裸 `<function=名字><parameter=...>` 块（缺 `<tool_call>` 外壳，训练转录体里的异框架格式污染）。原解析器按「malformed → 透传为文本」处理 → agent 收到纯文本当最终回答 → **回合静默终止**
- **根因链**：tools=53 已渲染进系统块（requests.jsonl 实锤）→ 模型输出漂移 → 解析器不认 → 客户端判停
- **补丁**（`patches/0001-tool-call-drift-salvage.patch`，改 `src/serve/tool_call_parser.cpp`）：
  - 无外壳时尝试挽救：块结构完整（parse_one_tool_call 复用）+ 导语 <400 字符且无代码围栏/缩进代码痕迹 + 块外仅空白与游离 `</tool_call>`（剥除）→ 解析为结构化 tool_calls；任一条件不满足维持原文透传，零误伤
  - 流式过滤器同步扣住 `<function=`（false positive 无害：finish 按终态分类回冲）
- **测试**：解析器 14/14（原 7 + 新 7：挽救/导语/围栏/畸形/尾随散文/流式扣留/字节还原），MSVC 2022 cl 直接编 test+parser 目标验证
- **端到端**（patched 二进制）：漂移诱导 → tool_calls/`tool_use` + `finish_reason=tool_calls`/`tool_use`、零泄漏；规范 `<tool_call>` 路径回归 ✓；纯文本回归 ✓；OpenAI 与 Anthropic 双协议 ✓
- **部署**：ninja 重链 apps/ninfer-serve.exe（增量 2 步）+ 滚动重启，服务恢复完整形态

### Phase 10 — 只读日志控制台 (2026-09-04)
- 需求：后端日志终端可见，方便管理。原则：**日志文件是唯一真相，控制台只做只读观众**（FileStream FileShare.ReadWrite）——关窗/Ctrl+C 任何时候都安全，绝不影响 serve（与坑#3 作业对象收割正好相反方向的教训：观众进程死了无所谓）
- `ninfer_console.ps1`：2s 增量追踪 task_err(SRV/红) + task_out(OUT/灰) + controller.log(CTL/青)，每 ~16s 打状态头（serve 状态 / draft 档位 / GPU 显存）；文件被轮转自动重开；初始回看各文件最后 8KB
- 桌面双击入口：`NInfer Console.lnk` → `ninfer_console.bat`（start 独立窗口）。家用机不挂常驻，按需唤出
- 编码坑：PowerShell 5.1 跑含中文的 ps1 必须 **UTF-8 BOM + CRLF**，否则 GBK 乱码

### Phase 11 — 第二种漂移形态：有外壳无闭合 (2026-09-04 02:00)
- **新形态**：`<tool_call> <function=Bash> <parameter=command> ... <parameter=description> ... </tool_call>`——有外壳但 `</parameter>`/`</function>` 全缺（zcode req48 实测 `finish=stop_token`，漂移漏成文本 → 回合终止）
- **扩展挽救**：严格解析失败时二次挽救——定位 `<function=name>`，参数按"下一个 `<parameter=` 标记或闭合标签先到者"切边界取值，不需要闭合标签。**截断块（无 `</tool_call>`）明确拒绝**（半截命令不能执行）
- 挽救安全门与裸块路径共用：导语 <400 字符、无代码围栏/缩进代码
- 单测 18/18（新增：真实畸形样例挽救/无参数拒绝/长导语拒绝/截断拒绝），E2E 漂移诱导 → tool_calls {"city":"Paris","days":2} ✓
- **插曲：controller.log 文件锁**——桌面控制台窗口（PowerShell -NoExit）以默认 FileShare 读日志时阻塞了 Add-Content，控制器心跳停摆 26 分钟但进程活着。教训：任务状态码/进程活着 ≠ 业务活着，**心跳才是真相**（SKILL 已有此条，再次验证）
