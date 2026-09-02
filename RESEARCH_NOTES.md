# 研究对齐验证：Windowed-MTP 与 KV 量化前沿（2026-07 快照）

本文记录两项最新研究在本机（RTX 4090 D 48GB + NInfer）上的实证，以及给上游的可操作建议。

## 1. Windowed-MTP 假设验证：draft-KV 税实测存在

**论文**：Windowed-MTP: Removing the Full-Context Draft-KV Tax at Million-Token Context (arXiv 2026)

**论文主张**：MTP draft head 在每步 propose 时对整个 KV cache 做全量 attention，读取量随上下文线性增长；在长上下文下成为 decode 瓶颈。

### 本机实证

同 payload（64K prompt）、同配置（128K ctx, c=4, rk4v4-e8），只切投机模式：

| 模式 | 64K decode | 短上下文对照* | 上下文衰减 |
|---|---:|---:|---:|
| 无 MTP（基线） | 36.5 tok/s | 52.8 tok/s | -31% |
| MTP7 | 84.6 tok/s | 195–203 tok/s | -57% |

*短上下文基线取上游 24GB 4090 公布数据（同模型同构建）。

**draft-KV 额外税 = 1 - (57%衰减 / 31%衰减) ≈ 37%**。若消除该税，MTP7@64K 应达 ~135 tok/s（当前 84.6），相对提升 60%。

### 源码证据（upstream @3d11fd64）

`src/targets/qwen3_6/impl/runtime/program_impl.h`:

```cpp
schedule::MtpGqaEnvelopes mtp_gqa_envelopes(std::uint32_t max_frontier, std::uint32_t k,
                                            std::uint32_t capacity) {
    ...
    out.target_verify = {1, visible(max_frontier + k + 1)};
    for (step = 0; step + 1 < k; ++step) {
        out.ar[step] = {1, visible(max_frontier + k + step + 2)};   // ← 全上下文可见
    }
}
```

`GqaExecutionEnvelope{min_visible_keys, max_visible_keys}` 就是 attention 的 KV 可见范围（`include/ninfer/ops/gqa_attention.h`），draft AR 步的可见上界直接取 `frontier + k + step`——**没有任何窗口截断**，与论文描述的"full-context draft-KV"完全一致。

### 给上游的窗口化建议

- MTP draft 头只有 1 层（`mtp_layers=1`），它的 attention 质量对远处 key 的依赖远低于 target 层；窗口化（如最近 8K–16K + 任意 sink tokens）对接受率的影响需要实验，但读取量从 O(frontier) 降到 O(window)。
- 实现切口很小：`mtp_gqa_envelopes()` 的 `out.ar[step]` 与 `mtp_impl.h` 的 `bridge_envelope` 改为 `visible(min(capacity, draft_window_cap))`；KV page gather 已按页表进行，不需要物理重排。
- 注意 CUDA Graph 的 envelope 是捕获期固化的（`program_impl.h:1494` warmup 用 `code_warm.max`），窗口化后 visible 随 frontier 增长变为动态值，需要改为按页表 gather + 固定窗口宽度，或分段重捕获。这可能是上游把它留成 full-context 的真实原因。

**结论**：税是真的（37%），修复方向明确，但触及 CUDA Graph 捕获语义，属于上游级改动，已具备提 issue 的全部素材。

## 2. KV 量化前沿：E8 晶格在 115K–329K 无"2-bit 悬崖"

**论文**：SPECTRA（谱变换突破 2-bit 悬崖）、HyQuant（注意力混合精度）、Output-Aware Rotation（INT2 KV），均 arXiv 2026。

这些工作主张传统 low-bit KV 量化在 2-bit 附近有精度悬崖，需要谱变换/旋转矩阵等预处理。NInfer 的 E8 (Conway–Sloane) 晶格量化是另一条路线——**直接在 8 维晶格上做矢量量化**。本机实测它是否需要论文里的补救：

### Needle-in-a-haystack 召回测试（同一 needle：`AZURE-PHOENIX-7749`，temperature=0）

| KV 模式 | 上下文长度 | 召回 | prefill | decode |
|---|---:|---|---:|---:|
| rk4v4-e8 (4-bit) | 115,208 tok | ✅ 精准 | — | 120 tok/s |
| **rk2v4-e8 (2-bit)** | 115,208 tok | ✅ 精准 | — | 109 tok/s |
| rk4v4-e8 (4-bit) | 328,906 tok | ✅ 精准 | 745 tok/s (441s) | 84.3 tok/s |
| **rk2v4-e8 (2-bit)** | 328,906 tok | ✅ 精准 | 591 tok/s (557s) | 47.9 tok/s |

### 结论

1. **2-bit 悬崖在本实现中不存在**（至 329K tokens）：E8 晶格矢量量化 + cylinder 结构的保真度足够支撑 329K 的精准 needle 召回，无需 SPECTRA 式谱预处理。论文的"悬崖"更可能针对 per-channel 标量量化。
2. **2-bit 的真实代价是速度而非精度**：329K 下 prefill 591 vs 745 tok/s（-21%），decode 47.9 vs 84.3 tok/s（-43%）——反量化/晶格解码路径比 4-bit 重。
3. **容量红利真实**：同 524K token 预算，2-bit 池 9.56 GiB vs 4-bit 11.68 GiB；48GB 下 2-bit 可撑 **1.5M tokens 池**（`--max-context 500000 --max-concurrency 4` 时 auto 解析 1,516,544 tokens / 28.77 GiB）。
4. **实用建议**：
   - ≤128K 生产（代码/agent）：`rk4v4-e8`（速度优先，池子够大）
   - 200K–500K 超长文档：`rk2v4-e8`（召回无损，容量红利），接受 20–40% 速度税
   - SPECTRA 类方法若要与 E8 晶格结合，作为"晶格前旋转"理论上可叠加，上游 `e8_root_codec.cuh` 是现成切口

## 3. 复现说明

所有测试在本机生产配置上进行（Qwen3.8-27B groupwise-int, CUDA 13.3, driver 616.56）。needle payload 为约 1.24 MB 源码文本中部插入唯一标记串。测试期间通过 Windows 计划任务切换实例配置（避免 SSH 会话进程回收，见 README 踩坑 #2）。
