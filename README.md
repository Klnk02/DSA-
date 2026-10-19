# DSA-：Intel DSA 对沙箱的影响性探索

> **当前入口（2026-10-19修订）**：[当前结论、证据边界与下一步](CURRENT_STATUS.md)。下文保留实验演进，标为历史的章节不用于当前定量结论；真实 snapshot DSA 端到端 A/B 尚未完成，已有报告二进制按历史快照保留。

> 目标：以 Intel DSA 为参照，量化「数据搬移（mem2mem / disk↔mem）硬件卸载」对沙箱（Firecracker microVM）关键路径的实际收益，作为评估「鲲鹏（ARM）是否需要实现 DSA 等价功能」的决策依据。

> 本文档与仓库内脚本同步维护，脚本变更时需一并更新对应章节。

## 〇、背景与定位

- **终极问题**：鲲鹏（Kunpeng/ARM）目前**没有** Intel DSA 这类「内存操作卸载引擎」（memcpy/memfill/compare/delta/CRC 的硬件加速）。要不要补这个能力？补哪些操作？——本项目聚焦其中的**数据搬运**（memcpy，即 mem2mem / disk↔mem 卸载）。
- **本项目做法**：在 Intel Sapphire Rapids（8480+）机器上实测 DSA 的加速收益，把 DSA 当作「价值标尺」，量化它到底值多少。
- **边界澄清**：鲲鹏 KAE 已具备**加解密 + zlib/gzip 压缩**加速，故压缩不是缺口；本项目聚焦 DSA **独有的数据搬移**能力（mem2mem / disk↔mem 卸载），memfill/compare/delta/CRC 不在研究范围。
- **交付形态**：价值评估报告 + **软件替代分析**（鲲鹏 NEON/SVE 软件能补上多少，作为「要不要造硬件」的必要对照）。

## 一、贯穿全程的三个核心判断

1. **DSA 有盈亏平衡点，不是处处划算**。CPU 在大块连续内存上 memcpy 已经能到几十 GB/s，而 DSA 每次提交有固定开销（构造描述符、写提交、等完成/轮询）。小尺寸下 DSA 反而不如 CPU——基准阶段最重要的任务就是测出这个"盈亏平衡尺寸"。

2. **沙箱值钱的是「并发」和「批处理」，不是单发**。沙箱平台是成千上万 VM，单进程测单次提交的极限吞吐意义有限。真正贴近沙箱的是 shared 队列 + ENQCMD 的并发共享，以及把海量 4KB 页操作打包成 DSA 的 Batch 描述符。

3. **压缩不是本次重点**。DSA 本身不含压缩（压缩在 IAA/QAT），而鲲鹏 KAE 已能做压缩；沙箱内存层压缩用 LZ4/zstd，也与 IAA 的 Deflate 对不上。故本项目聚焦 DSA 独有的「数据搬移」（mem2mem / disk↔mem 卸载），memfill/compare/delta/CRC 与压缩一并留作旁支、不在研究范围。

## 二、目标机环境基线（已探测）

| 项目 | 值 |
|---|---|
| CPU | Intel Xeon Platinum 8480+（Sapphire Rapids），224 vCPU，2 NUMA |
| DSA | `dsa0`@NUMA0、`dsa2`@NUMA1（各 4 engine），**已启用** |
| IAA | `iax1`、`iax3`，**已启用**（被 `iaa_crypto` 占用） |
| 内核 | 6.14.0，`CONFIG_INTEL_IDXD=m`、`CONFIG_INTEL_IDXD_SVM=y` |
| 指令集 | 支持 `enqcmd` / `movdir64b` |
| IOMMU | `intel_iommu=on,sm_on`（**不含 `iommu=pt`**，否则 PASID 挂载失败） |
| 工具链 | accel-config 4.1.8 已装；DML 已装（user-local）；libaccel-config 已源码补装（user-local，DML 硬件路径依赖） |

**关键结论**：DSA 已启用，`/dev/dsa` 节点已生成，可进入基准测试。

## 三、完整路线（6 阶段，逐级收窄）

| 阶段 | 做什么 | 产出 | 回答的问题 |
|---|---|---|---|
| 0 环境探测 ✅ | 摸清 DSA/IAA/驱动/工具链 | `check_dsa_env.sh` + 结果 | 硬件在不在、能不能用 |
| 1 启用配置 ✅ | 启用 DSA、配 dedicated+shared 队列 | `setup_dsa.sh` | 把 DSA 变成可调用状态 |
| 2 核心基准 | DSA vs CPU 数据搬运（memcpy）对比 | 加速比矩阵 | memcpy 多大尺寸才划算 |
| 3 场景建模 | 把沙箱热点抽象成 DSA 微负载 | 贴近沙箱的 benchmark | 沙箱典型负载 DSA 有没有戏 |
| 4 沙箱集成 | 把 DSA 接进 Firecracker 1-2 条热路径 | 接入 DSA 的沙箱 + 端到端数据 | checkpoint/resume 实际快了没 |
| 5 结论报告 | 汇总 + 软件替代分析 | 报告 + 建议 | 鲲鹏要不要做等价功能、软件能补多少、优先做哪些操作 |

### 阶段 2 基准维度（核心）

- **操作**：本项目聚焦 **memcpy（数据搬运，mem2mem / disk↔mem）**。memfill、compare、delta、CRC32C、Dualcast 为 DSA 能力全集，不在本项目研究范围。
- **对比轴**：吞吐（GB/s）、延迟（us）、CPU 占用率、批量大小、dedicated vs shared。
- **关键输出**：一张「操作 × 模式 × 尺寸 → 加速比」矩阵 + 盈亏平衡点。
- **双用途**：这组 CPU baseline 同时是「软件替代分析」的参照——ARM 上数据搬运（memcpy）只能用 NEON/SVE 软件做，需用同样 benchmark 在鲲鹏上跑出软件基线，与 DSA 硬件值对比，得出「软件能补多少、硬件还有多少额外价值」。

### 阶段 2 初步实测（memcpy/fill，2026-10-12）

单发（dedicated 队列）CPU vs DSA 吞吐对比。原始数据见 [dsa_bench_results.csv](results/stage2-benchmark/dsa_bench_results.csv)，完整吞吐矩阵见 [memcpy_fill_cpu_vs_dsa.csv](results/stage2-benchmark/memcpy_fill_cpu_vs_dsa.csv)。

**memcpy 盈亏倍率（vs CPU）：**

| size | CPU memcpy | DSA memcpy | 盈亏 |
|---|---|---|---|
| 4KB | 63.4 Gi/s | 0.81 Gi/s | 慢 78× |
| 64KB | 36.1 | 9.94 | 慢 3.6× |
| 256KB | 35.9 | 19.7 | 慢 1.8× |
| 1MB | 26.8 | 26.2 | 打平 |
| 2MB | 16.4 | 27.4 | 反超 1.7× |

- **DSA 单发固定开销 ~4.8us**（fill ~3.7us）：构造描述符 + 提交 + busy-poll 等完成。
- **盈亏平衡 ≈ 1MB**：2MB 时 CPU 因跨 NUMA 访问掉到 16.4 Gi/s，DSA 稳在 27.4 Gi/s 反超。fill 同构（盈亏平衡 ~1MB，2MB 时 DSA 27.9 vs CPU 39.5，fill 上 CPU 更强）。
- **结论**：小尺寸（<256KB）单发 DSA 失效，沙箱 4KB 页操作必须靠 Batch；大块（≥1MB）memcpy 才划算。
- **踩坑**：DML 硬件路径运行时依赖 `libaccel-config.so`，缺失会静默返回 status=16、数据不搬（此前「350ns + Ti/s」假数据的根因）。另：DML 高层 API 是同步 busy-poll（CPU 占用率 ~100%，未释放），真异步需切低层 `dml_submit_job`，留阶段 3。

### 阶段 3 初步实测（异步 CPU 释放，2026-10-12）

低层 C API 四种模式对比（2MB × 2000 = 4GB），完整数据见 [results/stage3-async/cpu_offload_async.csv](results/stage3-async/cpu_offload_async.csv)。

| 模式 | 占用率 | 每 GB CPU 时间 |
|---|---|---|
| CPU memcpy | 99.7% | 82.9ms |
| DSA 同步 busy-poll | 100% | 35.0ms |
| **DSA 异步 + sleep** | **8.2%** | **8.5ms** |
| DSA 异步 + umwait | 100% | 35.1ms |

- **`dml_submit_job` 是真异步**：提交后 CPU 立即返回、不 busy-poll。异步 + 让出 CPU 后，搬运占用的 CPU 时间从 82.9ms/GB 降到 8.5ms/GB，**释放率 89.76%**。
- **umwait 只省电不释放**：CPU 仍被占用（占用率 ~100%），真释放靠「让出 CPU」而非低功耗等待。
- **换算到目标**：vCPU 可用资源提升 = 搬运占比 × 89.76%。若搬运占 vCPU 总负载 6%~11%，则提升 ≈ 5%~10%（搬运占比待阶段 4 实测）。**（2026-10-14 口径修正）**此推导过于乐观：89.76% 是 mem2mem 纯拷贝的释放率，真实链路里 DSA 只够得着搬运 CPU 的一部分，修正公式与达标条件见 4.4。

**4KB 页操作 Batch 打包**（冷数据，16MB 连续 4KB 块，`dsa_batch_4k.c`，数据见 [results/stage3-batch/batch_4k.csv](results/stage3-batch/batch_4k.csv)；阈值经加密采样 N=48/56/72/96 + 每组 3 次重复复测，2026-10-12）：

| batch 大小 N | 吞吐 | vs CPU(8.55 GB/s) |
|---|---|---|
| 4 / 8 / 16 / 32 | 2.2~4.6 GB/s | 0.25~0.53×（慢） |
| **48** | **43.9 GB/s** | **5.1×（阈值）** |
| 56 / 64 / 128 / 256 | 49.0 / 53.0 / 75.6 / 94.3 GB/s | 5.7× / 6.2× / 8.8× / 11.0× |

- **存在尖锐批量效率阈值 N≈48**（32 < N ≤ 48）：N≤32 时 DML/DSA 未真正批量（逐个提交，仍慢）；N=48 起吞吐跳变 4.60 → 43.94 GB/s（≈9.5×），≥48 后进入渐进区（43.9 → 94.3，越大越划算）。
- **对沙箱是利好**：单发 4KB 慢 78×，但攒够 ≈48 个 4KB 页（约 192KB）打包，DSA 反超 CPU 冷拷贝 5~11 倍。沙箱的快照/dirty-page/zeropage 天然是批量页操作，适合攒批。

## 四、沙箱的 DSA 接入点

| 沙箱环节 | 底层操作 | DSA 能力 |
|---|---|---|
| 快照生成「ExportToDiff 拷数据」 | 拷贝（mem2mem） | memcpy |
| 快照恢复（磁盘→内存） | 拷贝（disk→mem） | memcpy |
| 零拷贝 / Direct I/O 搬移 | 拷贝（mem2mem） | memcpy |

> ⚠️ 口径限定（2026-10-19 复核，见 [qa.md](qa.md) Q9/Q10）：上表是**能力**对照，不等于**现状可介入**。
> 现状 snapshot 的两个方向（mem2disk：`process_vm_readv`+`pwrite`；disk2mem：ublk/overlaybd 读 + 恢复缺页）
> 数据搬运全在**内核**里完成，DSA 只能搬提交者本进程地址空间内的内存 ⇒ 结构上无法介入，
> 「快照恢复（磁盘→内存）= memcpy」在现状路径上**不成立**（详见 §4.2 源码级链路）。

> 以下为「已识别但不在本研究范围」的接入点（涉及 memfill/compare/delta，本次范围排除）：
>
> | 沙箱环节 | 底层操作 | DSA 能力 |
> |---|---|---|
> | CoW / dirty page 检测 | 比较 + 差异 | compare + delta |
> | guest 内存初始化 / zeropage | 填零 | memfill |

### 阶段 4 规划（沙箱集成）

**目标**：把 DSA 接入 Firecracker 的 1~2 条 mem2mem 热路径，端到端量化「搬运占比」与「实际收益」，并回答两个阶段 3 遗留的技术前提。

**阶段 3 带入的遗留问题（接入前必须先回答）：**

1. **散落页 batch 是否仍有效**：阶段 3 的 `dsa_batch_4k.c` 用「16MB 连续内存切出的连续 4KB 块」打包，src/dst 地址连续；但 snapshot 的真实页是**物理散落**的（虚拟连续、物理不连续）。DSA batch 描述符每个子操作本就是独立 src→dst 区间，理论上支持任意地址，但**尚未实测散落页打包是否仍拿 5~11×**——这是 Data Stream Offload 的物理前提。
2. **页温度决定 5~11× 的适用条件**：阶段 3 的 5~11× 建立「冷拷贝基线 8.55 GB/s」（16MB 超出 cache）之上；若 snapshot 的页是仍在 cache 的「热页」，CPU 基线更高（单发 4KB 热基线达 63.4 GB/s），DSA 反超幅度会缩水。需测真实 snapshot 场景的页温度，确定反超倍数的可信范围。

**分步：**

| 步 | 做什么 | 产出 |
|---|---|---|
| 4.1 场景建模 | 写 snapshot 微负载 benchmark：散落页 batch + 冷/热页 CPU 基线 | 回答上面两个遗留问题 |
| 4.2 搬运占比 | 在 Firecracker 跑典型负载，perf/插桩测 mem2mem 占 VMM vCPU 总负载比例 | 搬运占比数值 |
| 4.3 FFI PoC | Rust 调 `dml_submit_job` 提交一个 DSA 作业并等完成 | 证明 Rust→DSA 链路通 |
| 4.4 接入端到端 | 选 1~2 条热路径接 DSA，A/B 对比 checkpoint/resume | 端到端收益数据 |

**4.1 场景建模初步实测**（2026-10-12，`dsa_scene_4k.c`，数据见 [results/stage4-scene/scene_4k.csv](results/stage4-scene/scene_4k.csv)）：

| 页状态 | CPU 基线 | DSA batch 反超（N=48~256） |
|---|---|---|
| 冷页（128MB flush） | 9.22 GB/s | 4.8~10.6× |
| 热页（64KB cache 常驻） | 38.57 GB/s | 1.1~2.5× |

- **散落页 batch 物理前提成立**：N≥48 后散落/连续吞吐 ≈ 1.00×（物理散落的 4KB 页打包进 batch 不损失吞吐），Data Stream Offload 物理可行。
- **5~11× 只在冷页成立**：热页 CPU 靠 cache（38.57 GB/s，热/冷比 4.18×），DSA 反超缩水到 1~2.5×（仍正收益）。snapshot 实际收益取决于页温度分布。
- **异常待留意**：N=32 散落（11.88 GB/s）反比连续（4.50）快 2.64×，发生在负收益区不影响结论；N=64 散落单点波动大（38.8~54.3）。

**4.2 搬运占比初步实测【历史：真实路径数据受污染，勿引用其942与ext4约55%】**（2026-10-12，AgentENV snapshot 的 memory 搬运链路，数据见 [results/stage4-sandbox/snapshot_offload.csv](results/stage4-sandbox/snapshot_offload.csv)）：

> 对象：kvcache-ai 开源的 AgentENV（Firecracker 沙箱平台，Kimi K3 训练用）。本步回答「snapshot 里 mem2mem 搬运到底占多少 CPU、走哪条路径」。

**机制（源码级确认）**：AgentENV snapshot 是「KVM dirty page tracking **增量快照**」，非全量 memcpy。搬运链路：orchestrator（`server` 进程，`tokio-rt-worker` 线程）用 `process_vm_readv` 从 firecracker 进程读 dirty 匿名内存 → `pwrite` 写入 OverlayBD commit 文件（ext4）。

**搬运 CPU 量化（5 次取平均）**：

| 指标 | 值 |
|---|---|
| pause 固定开销（0MB dirty） | 53.32 ms |
| pause 256MB dirty | 288.94 ms |
| 纯搬运（256MB） | 235.62 ms |
| **搬运 CPU 成本** | **942 ms/GB**（对比：CPU memcpy 82.9、DSA 异步 8.5） |

**搬运链路符号构成（perf 采样 orchestrator + ublk，只包 pause）**：

| 符号 | 占比 | 属性 |
|---|---|---|
| ext4 文件系统逻辑（`ext4_da_write_begin`/`block_write_begin`/`write_end`） | ~55% | 持久化簿记，非数据搬运 |
| `copy_page_to_iter`（`process_vm_readv` 的跨进程拷贝） | 10.18% | **数据搬运** |
| `copy_page_from_iter_atomic`（写 page cache 拷贝） | 2.46% | **数据搬运** |
| 页表遍历 + 元数据 + 状态机 | ~32% | 逻辑 |

**关键口径修正（踩坑）**：

- ❌ 曾误判「snapshot 无 CPU 搬运」：根因有二——(1) 早期用 `dd 写文件`制造 dirty，测到的是 overlaybd 文件系统 CoW，不是内存 snapshot；(2) perf 只采 `firecracker` 进程，漏掉真正搬数据的 orchestrator（`server`）进程。
- ✅ 准确表述：**显式 `memcpy` 符号占比 ≈ 0，但搬运路径真实存在**——现代 x86 数据搬运用 `rep movsb`（ERMS）这类硬件字符串指令，perf 符号树显示的是其上层封装 `copy_page_to_iter`/`copy_page_from_iter_atomic`/`copy_user`，而非符号 `memcpy`。
- ⚠️ 显性 copy 指令 ~12.6% 之外，**搬运仍有隐性成本（cache 污染、内存带宽争用）**：`rep movsb` 把冷数据拉进 cache 会拖慢 ext4 那 ~55% 的逻辑。这部分次生收益无法靠符号占比静态算，只能实测 DSA 卸载后的整体 CPU 下降。

**4.2 补充：DSA 卸载 memory copy 的 CPU 下降比例**（`dsa_snapshot_offload.c`，模拟「散落 dirty 页 → 连续 target」的 snapshot 搬运形态）：

| 配置 | DSA batch 每 GB CPU | vs CPU memcpy（112.7ms/GB） |
|---|---|---|
| 4KB + usleep 异步 | 155 ms/GB | -37%（假象） |
| 4KB + busy-poll | 24.4 ms/GB | **+78.4%** |
| 64KB + usleep 异步 | 9.4 ms/GB | +91% |

- **描述符构建其实很便宜**（~100ns/页 < memcpy 467ns/页），DSA 卸载 4KB 页本身有正收益：busy-poll 下 CPU 时间下降 78.4%。
- **「4KB 负收益」是 usleep 异步等待的假象**：`usleep` 每次 ~25µs 系统调用开销 × 682 批 ≈ 130ms/GB，把收益吃光；去掉 usleep 改 busy-poll 后立即翻正（-37% → +78.4%），已用对照实验坐实。
- 阶段 3 的「batch 反超」与本次一致：都是无 usleep 的 busy-poll 口径（阶段 3 用 `dml_execute_job(BUSY_POLL)`）。

**异步等待的源码查证（AgentENV）**：

- AgentENV 的「搬运」本身是**同步**的：capture 阶段用 `process_vm_readv`（同步 syscall）阻塞等内核 copy 完成，**无异步等待逻辑**。
- 异步在另两个层面：写 commit 文件走 `io_uring`（`AsyncIoRing`），resume 走 `MAP_PRIVATE` + CoW 懒缺页（官方架构文档）。
- **对 DSA 的意义**：AgentENV 现状是同步 memcpy，本无异步等待问题；只有接 DSA 才引入「等待 DSA 完成」的抉择。而 AgentENV 已有成熟 io_uring 基础设施，若接 DSA 可用 io_uring/eventfd 等高效等待替代 usleep，避开 usleep 陷阱。

**修正后的阶段 4.2 结论**：DSA 卸载 4KB 页搬运有真实正收益（busy-poll 78.4%）；「负收益」是 usleep 假象，非 DSA 能力边界；真正开放的问题是「异步释放 CPU 需高效等待」，AgentENV 的 io_uring 可支撑。→ 转入 4.3/4.4 评估。

**4.3 干净全量定稿 + 模拟推断（2026-10-19，磁盘清理后重跑；[measure_copy_share.sh](measure_copy_share.sh) + [dsa_disk_scene.c](dsa_disk_scene.c)）**：

> 4.2 的 942 ms/GB 与「ext4 簿记 ~55%」是**磁盘满污染轮**数据（失败事件重试路径烧 ~4 倍 CPU：污染轮 5651 样本/事件 vs 干净轮 1450），以下为干净轮定稿口径：
> - **A 段（分母锚点）**：pause task-clock 差分回归 = **516 ± 74 ms/GiB + 固定 108.3 ms**（0/64/256/1024MB × 20 轮，R²=0.708，0/80 失败）。
> - **B 段（构成）**：perf -F4000 包 60 成功事件，87024 样本，符号 100% 解析——搬运段 = 读侧 `_copy_to_iter`（process_vm_readv 跨进程）**11.66%** + 写侧 `copy_page_from_iter_atomic`（pwrite）**2.20%** = **13.85%**（Wilson 95%CI [13.64, 14.07]）= 71.5 ms/GiB，单项最大已识别头；其余 = page cache/XArray 10.65% / memcg/LRU 8.24% / 页表 4.65% / 锁/RCU 3.37% / 长尾 57.8%。
> - **dsa_disk_scene 公平三结构**（60MB×15 冷页，`both` 方向，跨源对账 11 臂全 OK）：mem2disk Δ = 408.7 − 365.5 = **43.2 ms/GiB** → **推断比 = 43.2/516 = 8.4%**；disk2mem Δ = 423.6 − 387.4 = **36.2** → ≈**7.0%**（对称假设分母，待 MODE=resume 实测修正）。交叉验证：参照臂省比例 54.5%（85.0→38.7）× 真实搬运段 71.5 = 38.9/516 = 7.5%，同量级。
> - **净收益判据**：本轮60MiB微基准中，DSA新增staging两步相对软件一步，onCPU增加 **10.4% / 10.7%**。这支持该条件下新增搬运不划算；7%～8%仅为条件外推，不能表述为snapshot收益的严格上限。
> - 图：[report/figures/fig7_pause构成.png](report/figures/fig7_pause构成.png)；表：[results/stage4-sandbox/disk_scene_60mb_v6.xlsx](results/stage4-sandbox/disk_scene_60mb_v6.xlsx)（Sheet1 方案对比主表 + 推断比结论，Sheet2 详细 11 臂）。

> **4.3引用限制**：以上为已有汇总记录，原始copy_share与disk_scene日志本地待补归档。516是数据量相关整体CPU斜率；13.85%是另测窗口的copy符号占比；71.5是两者乘积而非独立实测。Wilson区间仅为样本计数近似，未涵盖事件间相关与分类误差。sw_1step是微基准read/write，非完整snapshot；disk2mem真实分母待测。

**4.4 A 臂基线初步实测【历史停用：674、近零固定项与下列收益情景不作为当前依据】**（2026-10-14，[baseline_pause_sweep.sh](baseline_pause_sweep.sh)，真实 AgentENV sandbox（整轮复用 1 个），repeat=20/档；0/64/256MB 已测、1024/2048 待补；原始数据见 [results/stage4-sandbox/baseline_sweep.csv](results/stage4-sandbox/baseline_sweep.csv)）：

> 方法与 4.2 对齐：`aenv pause` 触发、guest 内 `dd` 写 `/dev/shm` 制造 dirty；wall = `aenv pause` 命令耗时（含 CLI 往返，A/B 同口径）；CPU = `perf stat task-clock` 包住 orchestrator(server)。统计剔除 perf record 的 rep 1/8/15（profiling 使 wall +2.2s）与 64MB rep4（漏 dirty）。

| dirty | pause wall | server CPU（task-clock） |
|---|---|---|
| 0 MB | 169.6 ± 4 ms | ≈ 0 |
| 64 MB | 1170.8 ± 12 ms | 22.4 ms（噪声大：4.7~30） |
| 256 MB | 1163.6 ± 20 ms | 168.5 ± 22 ms |

- **发现 1 · pause wall 含 ~1s 与 dirty 量无关的固定等待**：64 与 256MB 的 wall 相同（sd 仅 1~2%，定时器级确定性），0MB 仅 170ms；期间 server CPU≈0（64MB 档 1s 窗口内仅 ~22ms、0MB 档 = 0）→ 推测为阻塞等待。**与 DSA 解耦、另线定位**（strace 待做）。含义：只减 CPU 的优化（含 DSA）预计不改变 pause 时长；推测 dirty ≳1GB（搬运时间超过固定等待）后 wall 才随 dirty 增长，待 1024/2048 档验证。
- **发现 2 · CPU 随 dirty 线性增长**：256MB 档折算 ≈ **674 ms/GB**（4.2 在另一沙箱测得 942——同量级、两次独立测量互为印证；不跨沙箱混比，A/B 臂内对比为准）。memory copy 子路径是 DSA 的直接优化对象，端到端收益受 filesystem/control-plane 占比限制。
- **发现 3 · 0MB 固定 CPU 开销 ≈ 0**：4.2 的 53.32ms 未复现（沙箱状态相关），本环境纯搬运 ≈ taskclk 全额。

**目标口径修正（承阶段 3「换算到目标」）**：

```
vCPU 可用资源提升 = 搬运占 vCPU 总负载 × DSA 可卸载占搬运 CPU 的比例
                                       ├ 保守 ~12%：仅拷贝符号（674 × 12.6% ≈ 85 ms/GB → 异步 DSA ~8 ms）
                                       └ 乐观 ~25%：memfd 连跨进程页表遍历一起省 + cache 污染消失（B 臂实测钉死）
```

| 搬运占 vCPU 总负载 | 可卸载 12%（保守） | 可卸载 25%（乐观） |
|---|---|---|
| 5% | 0.6% | 1.3% |
| 10% | 1.2% | 2.5% |
| 20% | 2.4% | 5.0% |
| 40% | 4.8% | 10.0% |

- **5~10% 目标需要「重快照节奏（搬运占比 20~40%）+ 乐观可卸载」同时成立**——一半取决于生产参数（快照周期 × 每次 dirty 量 × 沙箱密度），待确认。直观换算：搬运占 10% 总负载 ≈ 8480+ 上 ~33 GB/s 整机快照流量（1000 沙箱 × 1GB/30s 量级）。
- **B 臂任务重述**：把可卸载比例从 12~25% 区间钉成实测值（直接决定 5~10% 能否够着）；预期 pause CPU 降 9~25%、wall 持平（被 1s 等待掩盖）。
- **写盘段是更大的 CPU 杠杆**：ext4 ~55% ≈ 371 ms/GB vs 拷贝符号 ~85 ms/GB → C 臂（O_DIRECT）与 B 臂并排跑，归因分开。

### 阶段 4.5 · 透明插桩验证（leader 确认方向：不改 AgentENV 源码）

> **方案状态修订**：下列为历史方案设计，非全部已验证成果。anon/nonce的mmap/DSA臂从本地预取缓冲搬运，预取在计时窗外，checksum不保证跨臂guest内容一致；GET_DIRTY_LOG的副作用取决于目标模式，只GET不CLEAR不能保证零扰动。当前排序以[验证计划](CURRENT_STATUS.md)为准，先归档与修复观测失败处理、补resume分母，再评估专用实例回放；旧12%～25%预期及C1等效负载数值停用。

> 立项依据：A 臂基线已锚定真实链路成本（4.4）；leader 指示下一步不改 AgentENV 源码，全部通过 eBPF / procfs / pidfd / 运行态回放等**透明手段**验证。4.3（Rust FFI PoC）与 4.4（feature flag 三臂）后移，待本阶段出数后再启动。
> 表述纪律（延续评审反馈）：现象 → 观察；因果 → 推测；占比 → 标注口径（进程/系统）；能力边界 → 覆盖范围。实验现象不过度解释成机制结论，数据不夸大。

**跟踪线 A · 观测与口径闭合（小活，一次服务器行程带完）**

| 编号 | 做什么 | 工具 | 回答的问题 |
|---|---|---|---|
| A1 | pause 窗口按 syscall/符号归因：process_vm_readv / vfs_write / fsync 的耗时与字节数；copy_page_to_iter / copy_page_from_iter_atomic 精确计时 | [bpftrace_pause_attribution.bt](bpftrace_pause_attribution.bt) | 真实 pause 里 memory copy 子路径精确占多少（含 iovec 分布，为 batch 参数输入） |
| A2 | 全机 CPU 口径标定：/proc/stat 差分（user+nice+sys+irq+softirq+steal）vs 进程 task-clock，残差 ≈ kworker 回写 + jbd2 + irq（推测级） | [calibrate_cpu_scope.sh](calibrate_cpu_scope.sh) | copy 占比 26~32% 是进程口径上界，系统口径修正值是多少（4.4 遗留的严谨性问题） |
| A3 | ~1s 固定等待定位：calibrate pause 模式 `PERFREC=1`（perf record -a 看窗口内全机谁在干活）+ `strace -f -p $(pgrep -x server)` 看 fsync/futex/nanosleep | 同上 + strace | 1s 等待在等谁（4.4 发现 1 遗留） |

**跟踪线 B · 运行态回放（核心 = B 臂的体外预演）**

原理：运行态（pause 之前）guest 已制脏完、随即 idle，firecracker 的 guest RAM 就是 rw-p 的 COW 私有页（实测 mmap /dev/ublkbN）；pause 会把该 COW 段 flush+unmap（读源消失）——所以三臂回放在运行态做，外部进程重放同一段搬运，测「真实页分布下的 DSA 收益」，零源码改动。

| 编号 | 做什么 | 回答的问题 |
|---|---|---|
| B0 | 定位 guest RAM **三路**：路 0 **nonce 内容定位**（guest 每轮制脏前写 16B 随机数 `/dev/shm/.b0nonce`，host 侧全机 fc 进程可读大段内容搜索（运行态 guest RAM 是 rw-p COW 页，可读；pause 会 unmap 掉它）——命中进程即沙箱真身、命中段所在同文件连续段组即 guest RAM。共享服务器多 fc 并存且 patch 版命令行无 uuid 时 find_fc 失效，此为唯一可靠手段；且实测 AgentENV 后端是 mmap `/dev/ublkbN` 拆两段，匿名大小匹配亦失效；**段组 → 线性偏移映射用 foff 升序拼接**（布局约定无关），不再假设 hva 与线性偏移同序——实测段组 foff 逆序排布（低地址段 foff=3GiB），旧线性模型会整体错位 512MiB）；路 1 上游 Firecracker 的 memfd（`pidfd_getfd(2)` 取 fd → mmap 进回放进程只读）；路 2 匿名 mmap 按 slot 大小匹配推断基址。anon/nonce 模式均叠加每轮魔数文件内容级校验（miss 即 NOGO）；nonce 路再叠 **`.b0loc` 位置编码门禁**（guest 写 16 页页首编码 "B0LOC+页号"，probe 对每个 memslot 布局候选做 gpa→线性偏移→hva→内容端到端校验，≥12/16 命中才 GO，全灭 NOGO loc-miss——错位数据宁可失败不出） | RAM 定位路线 go/no-go（B 臂核心前提；B0 行留痕 `ram_src=nonce|memfd|anonmap` + `segs/order/loc/s0`） |
| B1 | 真实 dirty 页集合：**先读 AgentENV 源码确认 dirty-log 机制（bitmap vs ring、GET 之后是否自动清除）**；运行态经 pidfd_getfd 取得 KVM VM fd，只读 `KVM_GET_DIRTY_LOG` 提取脏页位图（**严禁 KVM_CLEAR_DIRTY_LOG——会破坏 AgentENV 增量快照链**；若 GET 返回空位图则退化为整段 RAM 回放，口径偏保守）。**自检修正**：原设计的 pagemap soft-dirty 是错的——guest 经 EPT 的写入不经过 host 页表，不会置位 soft-dirty。nonce 段组模式位图位→线性偏移换算需知 slot0 字节数 s0，**布局不猜、实测求解**：候选 s0 ∈ {总 RAM（单 slot）/ foff 序累计边界 / hole 边界} ×（匿名组）地址正逆双排列，经 `.b0loc` 内容校验裁定（详见 [qa.md](qa.md)）；每次 GET 前 memset 位图缓冲（KVM 只写该 slot 字数，防跨 slot 残留幻影页） | snapshot 真实脏页分布与页温（补 4.1 热页缩水的工况缺口） |
| B2 | 同一批页**三路**对比：① `process_vm_readv` 循环（= 现状基线，双模式原生路径）② 直映源 + CPU memcpy（memfd 模式本地 mmap 直映；anon 模式一次性预取进本地缓冲后 memcpy，计时只含搬运）③ 同源 + DML batch（N≥48，复用 `dsa_snapshot_offload.c` 的 batch 描述符），输出 CPU ms/GB | 真实页分布（含热页）下的 mem2mem 收益——可卸载比例 12~25% 区间的实测落点，并把「直映收益」与「DSA 收益」拆开归因 |
| B3（可选） | 回放 + pwrite 到 commit 文件 | 完整 mem2disk 链路回放 |

- 产物（已实现）：[`replay_offload.c`](replay_offload.c)（单进程工具，`--probe` 三路定位 guest RAM（nonce 内容定位优先 → memfd → anonmap 推断+魔数校验）+ KVM fd + 只读 dirty log，`--arm` 跑三路之一）+ `replay_run.sh`（全自动编排：起沙箱 → 每 round：写魔数 + exec 负载 → probe（运行态）→ 三路回放（运行态）→ pause → resume（留 wall 对账）→ ABBA 交错 → 配对统计；`SMOKE=1` 冒烟）。
  - 设计要点：① dirty-log 三机制兼容——GET 即清（AgentENV 消费后我们 GET 得空位图 → 退化 whole，口径偏保守）/ manual-protect（真实脏页）/ ring（EINVAL → 退化 whole）；**只 GET 严禁 CLEAR**（保护增量快照链），GET 与消费逐 slot 配对，每轮只 probe 一次。② 沙箱内存 ≤3584MiB（默认 `MEM_MIB=3584`）时 guest RAM 全在单 memslot，消除 slot1 顺序布局假设。③ 段式采样：whole/bitmap 超 `SUBSET_MB` 上限时取「随机段起点 × 连续 256 页（1MB）」而非纯随机页（dd 顺序写使 dirty 大段连续，纯随机会把 pvm 64KB iovec 合并退化成逐页 4KB readv），seed=round 确定性可复现。④ pvm 基线臂按 64KB iovec 合并粒度换算（假设值，`PVM_CHUNK_KB` 可覆盖，待 A1 bpftrace 校准）。⑤ 退出码协议：rc=2 checksum 不一致（编排层 fatal 中止）/ rc=3 DML 臂失败（该轮降级剔除 dsa，不中止）。⑥ RAM 访问三路：nonce 内容定位优先（`--nonce-hex`，guest 每轮制脏前写 `/dev/shm/.b0nonce` 16B 随机数——多 fc 并存且命令行无 uuid 时 find_fc 失效，16B 随机数全机内容搜索可同时定位进程真身与 RAM 段组；实测 patch 版后端为 mmap `/dev/ublkbN` 拆两段地址连续 VMA，匿名大小匹配失效）；无 nonce 时上游 memfd 优先（偷 fd 只读直映）；AgentENV patch 版为匿名 mmap，降级为 `/proc/<pid>/maps` 匿名映射按 slot 大小匹配推断基址（须传 `--guest-mib`，候选非唯一即拒绝）+ 每轮制脏前魔数文件（`DSAB0BASECALIBv1!` ×4096 = 16 页 tmpfs）内容级校验——大小匹配可能被 guard page 偏移骗过，魔数不会，miss 即 NOGO；anon 模式下 mmap/dsa 臂先一次性预取页集合进本地缓冲（计时窗口外，真实场景 orchestrator 直映持有指针零成本），pvm 臂仍走 `process_vm_readv`（它测的就是跨进程读本身）。⑦ **布局校准（错位防御）**：nonce 段组的「线性偏移→hva」映射 = 段组 foff 升序拼接查表（实测 foff 逆序排布，旧 base+off 线性模型整体错位 512MiB、tmpfs 制脏页恰好落被丢的 slot1）；memslot 布局（slot0 字节数 s0）不猜、probe 实测求解——候选 × `.b0loc` 位置编码端到端校验（≥12/16 命中即 GO，全灭 NOGO loc-miss 宁失败不出错数据）；求出的段组排列经 dump 文件尾 `#B0ORDER` 行传给臂进程（臂本地重发现排列未必与 probe 一致，如匿名组正/逆），臂 `load_pages` 后强制按 dump 重排再回放；whole 退化模式无映射校验力（全页集合任意排列都覆盖全部页），loc=n/a 如实留痕。
  - 状态：合成数据自测 27/27 通过（臂序排列与块序性质、统计输出与手算期望逐字节一致、probe 解析 pipefail 三分支、17 列 CSV 对齐）；待服务器实测出数。
- 已排除的不可行路线：LD_PRELOAD 拦不住 Go 裸 syscall；ptrace 拦截 syscall 无法干净做跨进程 DSA（数据先得经 kernel 拷一遍，等于没省）。

**跟踪线 C · 干扰实验（证明「释放 vCPU → 业务算力」）**

| 编号 | 做什么 | 回答的问题 |
|---|---|---|
| C1 | 合成负载预演（零改动）：同 NUMA 绑核 co-runner（固定工作量循环测吞吐），注入 A 臂实测等量 CPU 负载（674 ms/GB）vs DSA 等效（~76 ms/GB），对比快照窗口内 co-runner 吞吐保留率 | 打通「CPU 占用 → 业务降速 → 释放 → 恢复」因果链（预演版，推测级外推） |
| C2 | 真实版（等 4.4 B 臂落地）：周期性真实快照 vs 无快照，co-runner 对比 | 真实快照下的同机业务影响 |

> 口径提醒：被暂停沙箱自身的业务在 pause 期间整体停摆，vCPU 释放的可证明对象是**同机其他负载**；沙箱自身的收益路径是缩短 pause wall（~1s 等待，与 DSA 解耦）。

**排序与判定标准**

1. A2 + A3（一次服务器行程）→ 2. B0~B2（核心）→ 3. C1 → 4. A1（随时可做，与 B 并行）。
- 观察级：A/B/C 的直接测量数；推测级：残差归因（kworker/jbd2）、1s 等待机制、C1 因果链外推。
- 修正项：copy 占比补系统口径双数值；若系统口径下修 10~13%，PPT 与数据文档同步改。

**4.5 测试模型（自检后固化，B 线及后续 B 臂共同遵守）**

- **臂**：B2 三路——`process_vm_readv` 基线 / mmap 直映 + CPU memcpy / mmap + DSA batch（把「直映收益」与「DSA 收益」拆开归因）。
- **因子**：D = 64/256/1024MB（+0 对照，顺带补 A 臂 1024 缺口）；S = dd 顺序 dirty vs 真实负载 dirty（B1 捕获）；T = 热（运行态立即回放 COW 页）vs 冷（整段 RAM，保守口径）。
- **固定并记录**：CPU governor=performance；NUMA 绑定（server / DSA WQ / 回放进程 / commit 文件同 node，numactl 指定并留痕）；沙箱规格与 layer 深度（pause 次数）；后台负载（mpstat 验证 <1%）；perf 平均频率。
- **执行顺序**：同沙箱内臂间**交错随机化**（ABBA，每 5 轮换序）。教训：4.2 与 4.4 跨沙箱 40% 差异（942 vs 674 ms/GB）大于 B 臂预期效应（9~25%）——改源码 B 臂绝不允许跨天/跨沙箱对比，必须同沙箱配对。
- **样本量与统计**：n=30/臂（配对 t / Wilcoxon + 95% CI）；按 A 臂 sd≈13% 估，配对可检出 ~7% 差异（80% power）。主指标预注册：E1 搬运 CPU ms/GB（配对差）、E2 系统口径 copy 占比（A2）、E3 co-runner 保留率（C1）；wall 与符号构成列为次级。
- **Sanity 门**：DSA 回放后 perf 采样中 `copy_page_to_iter` 应基本消失；回放前后 guest 页 checksum 一致；B0 双路（memfd + anonmap 推断）都失败才触发 no-go 转向；nonce 路 `.b0loc` 布局校验全灭 → NOGO loc-miss（memslot 布局与预期不符，宁失败不出错位数据）。
- **口径限定**：C1 合成负载只复现 CPU 时间占用、不含内存带宽争用——保留率结论限 CPU 归因（推测级）。已知未测记录在案：DSA 多租户容量（8 engine vs 整机 33 GB/s 流量）、单机外推鲲鹏、写侧 kworker/jbd2（A2 在补）；resume 侧宿主开销已纳入 B 线 rounds 留痕（wall + server utime+stime 差分，窗口 = resume 返回点保守下界）。

## 五、脚本清单

### [measure_resume_baseline.sh](measure_resume_baseline.sh) — 独立resume基线（2026-10-19新增）

仿照measure_copy_share分开做A段CPU扫描与B段符号采样；两段均在同一运行中复用一个沙箱，pause/制脏在计时窗外。每轮轮转dirty档位和窗口顺序，失败保留退出码与stdout/stderr；输出使用唯一run目录，不覆盖已有服务器日志。

先在Linux目标机冒烟（SBX需处于运行态；没有SBX时可用TEMPLATE自动启动）：

```bash
SBX=<运行中的沙箱ID> REPEAT_A=2 REPEAT_B=1 DIRTY_MB_LIST="0 64" bash measure_resume_baseline.sh
# 全量默认0/64/256/1024MiB，各20轮；另各60次256MiB符号采样
SBX=<运行中的沙箱ID> bash measure_resume_baseline.sh
```

- `WINDOWS="return touch"`：分别测resume命令返回，以及resume后exec校验文件长度并顺序读回全部制脏文件。后者显式触发该文件的懒加载，含exec、guest读文件与调度成本，不等于纯disk2mem或全RAM恢复。`0MiB`仍创建空文件并执行相同就绪/读取路径，作操作性控制，不保证guest零dirty。
- 默认`SCOPE=process`，`auto`仅为兼容别名，**不自动退回全机采样**。三类角色只在候选唯一时自动选择；多候选需用`SERVER_PIDS/UBLK_PIDS/FIRECRACKER_PIDS`指定已确认属于目标路径的PID。唯一候选也不是沙箱归属的内容级证明，需结合部署确认。PID、starttime及边界线程集合变化则事件无效；若resume新建FC，当前进程方案不能给出完整占比，需另做目标cgroup/生命周期归属方案，不以全机数据冒充。
- 关闭继承避免重叠计数；进程集合外的其他进程不计入，但系统异步worker不在本口径内，边界之间瞬态线程也可能漏掉。共享server/ublk内其他请求无法靠PID区分，运行时应暂停并发管理操作，优先专用测试实例。显式`SCOPE=system`仅诊断，分析器禁止将其标为可推演输入。
- 每事件前对相同目标进程做`BG_SEC=1`秒空闲CPU测量，按操作窗口时长估算背景比例；超过`MAX_BACKGROUND_RATIO=0.10`则事件无效。这是噪声筛查，不是背景扣除或零干扰证明。
- 默认不清宿主page cache、不改sysctl、不接触dirty-log。pause/resume多轮会改变layer/cache状态；dirty量是请求制脏量，不能当作物理读盘量。`wall_ms`包含perf/辅助进程启动退出，不能直接与旧CLI-only wall混比。
- 把`analyze_resume_baseline.py`与shell脚本放在同一目录，结束时自动生成`composition.md/csv`（互斥操作符号类别、合并占比、事件均值、按事件重采样95%区间）、`composition_events.csv`、逐符号明细和`inference_inputs.json`。B段显式采样`cpu-clock`，`--no-children`避免父子栈重复计数；长尾和未解析样本保留，禁止把未解析当copy=0。分类是self符号归类，不是精确syscall耗时或DSA可卸载比例，需要依据真实长尾继续审计。
- 默认推演质量门禁：每组至少20个有效符号事件、20000样本、同窗口/dirty/PID的至少20个A段事件、未解析≤5%、有已解析内核样本，且报告解析无异常/已识别丢样。通过仍需确认PID归属及共享进程无并发请求；不是已经证明无干扰。
- 推演只能条件性地用类别占比乘**同窗口、同dirty、同PID集合的平均总CPU**，不能直接乘dirty回归斜率。OLS与事件重采样区间不覆盖所有分类/环境误差。原始产物写入唯一run目录，既有服务器日志无需重做或覆盖。

> 各阶段实测结果留痕见 [results/](results/README.md)，本文档只保留结论摘要与指针。

### [check_dsa_env.sh](check_dsa_env.sh) — 环境探测

- **性质**：纯只读，无副作用。
- **探测内容**：系统信息、DSA/IAA 硬件、idxd 驱动、设备与工作队列、accel-config/DML 工具链、IOMMU、大页/NUMA。
- **用法**：`bash check_dsa_env.sh`

### [setup_dsa.sh](setup_dsa.sh) — 启用并配置 DSA

- **性质**：改设备状态（仅 `dsa*`，不碰 IAA 的 `iax*`），需 root。
- **配置**：每颗 DSA 配 1 个 dedicated 队列（`wq{idx}.0`，独占 `engine{idx}.0`）+ 1 个 shared 队列（`wq{idx}.1`，ENQCMD/SVA 共享 `engine{idx}.1`），其中 `{idx}` 为设备编号（`dsa0→0`、`dsa2→2`）。
- **用法**：`sudo bash setup_dsa.sh`（幂等，可重复执行）。
- **三个前置条件（踩过的坑）**：
  1. 内核启动参数**必须去掉 `iommu=pt`**，否则 PASID 挂载失败，user 队列无法启用。正确为 `intel_iommu=on,sm_on`。
  2. user 队列**必须显式 `--driver-name=user`**，否则 `enable-wq` 报 `IDXD_SCMD_WQ_NO_DRV_NAME (0x80200000)`。注意 `driver_name ≠ name`，是两个不同字段。
  3. shared 队列**必须显式 `--threshold`**（>0 且 ≤ wq-size-1），否则报 `threshold not set (0x80060000)`。dedicated 不需要 threshold。
- **回滚**：

```bash
sudo accel-config disable-wq     dsa0/wq0.0
sudo accel-config disable-wq     dsa0/wq0.1
sudo accel-config disable-wq     dsa2/wq2.0
sudo accel-config disable-wq     dsa2/wq2.1
sudo accel-config disable-device dsa0
sudo accel-config disable-device dsa2
```

### [baseline_pause_sweep.sh](baseline_pause_sweep.sh) — 阶段 4.4 · A 臂基线扫描

- **性质**：零代码改动，纯测量（真实 AgentENV snapshot pause），方法与 4.2 对齐（`aenv pause` + `perf stat task-clock` 包住窗口，orchestrator 进程名 `server`）。
- **内容**：整轮复用一个沙箱（pause → resume → 重打 dirty 循环，TTL 拉长到 12h）；dirty 量扫描（默认 0/64/256/1024/2048 MB，0 为固定开销对照组）× repeat（默认 20）；guest 内 `dd` 写 `/dev/shm` 制造 dirty；每档抽样 `perf record`（server + uvm-ublk-daemon）留符号构成。
- **用法**：`TEMPLATE=<模板名> bash baseline_pause_sweep.sh`；短版 `REPEAT=3`；OCI 镜像 `COLD=1 CPU_COUNT=4 MEM_MIB=8192`（guest 内存 ≥ 最大 dirty 档 ×1.5）；跑完删沙箱 `CLEANUP=1`。输出 `results/stage4-sandbox/baseline_sweep*.csv` + `perf/`。
- **口径**：wall = `aenv pause` 命令耗时（含 CLI 往返，A/B 同口径）；CPU = orchestrator(server) 的 task-clock（对照 4.2 的 942 ms/GB）；dirty 只写 guest tmpfs，禁止写盘（overlaybd CoW 坑）。汇总自动剔除 `perfrec` 行（profiling 使 wall +2.2s）并打印「疑似漏 dirty」告警（dirty>0 但 taskclk≈0）。

### [bpftrace_pause_attribution.bt](bpftrace_pause_attribution.bt) — 阶段 4.5 · A1 路径归因

- **性质**：eBPF 透明插桩，零源码改动，纯只读观测，需 root + bpftrace。
- **内容**：按进程名归因 `process_vm_readv`（耗时/次数/字节数）、`vfs_write`（kprobe，同时覆盖 pwrite 与 io_uring 提交路径）、`fsync/fdatasync`（1s 等待若源于落盘在此显形）；对 `copy_page_to_iter` / `copy_page_from_iter_atomic` 做 kretprobe 精确计时（真实 memory movement 直接计量）。
- **用法**：`sudo bpftrace bpftrace_pause_attribution.bt`，另开终端触发一次 `aenv pause`，Ctrl-C 出汇总（ns/1e6=ms）。一次归因一个 pause 窗口。
- **注意**：`copy_page_from_iter_atomic` 在部分内核被内联，probe 报不存在则删对应段，用 `perf record -a -g` 兜底。

### [calibrate_cpu_scope.sh](calibrate_cpu_scope.sh) — 阶段 4.5 · A2 全机口径标定

- **性质**：零源码改动；scene 模式只读 + 运行既有二进制，pause 模式触发一次 pause。
- **内容**：`/proc/stat` 差分（user+nice+sys+irq+softirq+steal）= 全机 CPU，对比进程侧（scene：`perf stat task-clock` 包住命令；pause：server 的 utime+stime 差分），残差 ≈ kworker 回写 + jbd2 + irq（推测级）。
- **用法**：`sudo ./calibrate_cpu_scope.sh scene -- ./dsa_disk_scene <参数>`；`sudo ./calibrate_cpu_scope.sh pause <sandbox_id>`；`PERFREC=1` 附带 `perf record -a -g`（A3 用），`SERVER_PID` 可覆盖 orchestrator 进程。每模式跑 3 次取中位。
- **产出**：进程 vs 全机 CPU 对比 + 残差；系统口径 copy 占比换算提示（mem2disk：56.0 / (56.0+158.9+残差ms/GB)）。

### [replay_offload.c](replay_offload.c) — 阶段 4.5 · B 线单进程回放工具

- **性质**：零源码改动的单进程工具；经 `pidfd_open`/`pidfd_getfd`（内核 ≥5.6，需 root）从 firecracker 进程取得 guest RAM 的 memfd 与 KVM VM fd；运行态（pause 之前）读 firecracker 的 guest RAM COW 页（pause 会 unmap 掉它），在 AgentENV 之外重放同一段搬运，测真实页分布下的收益。
- **内容**：`--probe` 模式（B0+B1）：`/proc/<pid>/fd` readlink 发现 memfd（前缀 `/memfd:`）与 KVM VM fd（`anon_inode:kvm-vm`）→ mmap 直映 → 只读 `KVM_GET_DIRTY_LOG` 提取脏页集合（**严禁 CLEAR**，三机制兼容见 4.5），超上限时段式采样，`--dump-pages` 落盘页表（nonce 模式尾行 `#B0ORDER foff|asc|desc` = 段组排列模型，臂进程据此重排映射）；nonce 段组模式位图换算先做 **memslot 布局求解**（候选 s0 × 匿名组正逆排列 → `.b0loc` 位置编码端到端校验 ≥12/16 命中才接受，全灭 NOGO loc-miss，方法详见 [qa.md](qa.md)）；`--arm` 模式（B2）：对同一批页跑 pvm 基线 / mmap+memcpy / mmap+DML batch（N≥48）三路之一，`--repeat` 次内部重复，每次搬完做 guest 页 checksum 自校验。
- **用法**：`sudo ./replay_offload --fc-pid <pid> --probe --dump-pages <file> [--subset-mb 256] [--seed 1]`；`sudo ./replay_offload --fc-pid <pid> --arm pvm|mmap|dsa --pages <file> [--repeat 5] [--cold 1]`。由 `replay_run.sh` 自动调用，无需手工单跑。GO 行（nonce 路）字段：`ram_src=nonce ram_mib=<实测MiB> segs=<段数> order=<foff|asc|desc> file=<后端> kvmvm=<0|1> magic=hit@0x<偏移> loc=<n/16|n/a> s0=<MiB>`——`loc=n/a` 表示 whole 退化（无映射校验力，口径保守）；NOGO `reason=loc-miss` = 所有布局候选校验全灭（错位防御触发，宁失败不出错数据）。
- **口径**：CPU = `CLOCK_PROCESS_CPUTIME_ID`，wall = `CLOCK_MONOTONIC`；13 列 CSV（arm,src,pages,mb,chunk_kb,flush,irep,cpu_ms,wall_ms,cpu_ms_per_gb,cpu_util,cksum,note）；退出码 0 正常 / 1 参数错 / 2 checksum 不一致（编排层 fatal）/ 3 DML 臂失败（编排层降级剔除该轮 dsa）。
- **编译**：`gcc -O2 -Wall -DUSE_DML -I"$HOME/.local/include" replay_offload.c -L"$HOME/.local/lib" -ldml -ldmlhl -luuid -pthread -o replay_offload`（无 DSA 环境去掉 `-DUSE_DML` 可跑 pvm/mmap 两臂）。

### [replay_run.sh](replay_run.sh) — 阶段 4.5 · B 线全自动编排

- **性质**：一键跑完 4.5 测试模型 B 线，零人工操作（规避手测误差）；`SMOKE=1` 冒烟模式（64MB × 2 轮 × 2 reps，验通路不出数）；编译自含。
- **内容**：起沙箱（`aenv start`，规格留痕）→ 编译 → 每 round：ABBA 臂序（块=5 轮，块间反转 + 块内确定性洗牌、块内同序）→ 写哨兵三件套（`.b0magic` 魔数 16 页 + `.b0nonce` 16B 随机数读回 + `.b0loc` 16 页位置编码，各带 stat 自检 WARN）→ exec 负载（dd 顺序写制造 dirty）→ probe（运行态）→ 三路回放（各 `ARM_REPEAT` reps，rc=2 fatal / rc=3 降级剔 dsa）→ `aenv pause` → `aenv resume`（留 wall 对账）；结束输出 round 均值 → 臂级 mean/sd/95%CI → **同沙箱配对差值**（缺臂轮自动配对降级并 WARN）；cksum≠1 行剔除；环境留痕 `replay_env.txt` 20 项（governor / NUMA 绑定 / 沙箱模板 / arms / dirty 档位 / pvm_chunk_kb 假设标注 / gcc / LD_LIBRARY_PATH 等）；resume 窗口同步留痕（wall + server 进程 utime+stime 差分 ms，进 `replay_rounds.csv`，窗口 = resume 返回点保守下界）。
- **用法**：`TEMPLATE=<模板名> sudo -E bash replay_run.sh`（全量：64/256/1024 MB × 30 轮）；`TEMPLATE=<模板名> SMOKE=1 sudo -E bash replay_run.sh`（冒烟）。可调参数：`DIRTY_MB_LIST / REPEAT / ARM_REPEAT / ABBA_BLOCK / FLUSH / SUBSET_MB / PVM_CHUNK_KB / BATCH_N / DSA_WAIT / COLD / CPU_COUNT / MEM_MIB / CLEANUP / FIRECRACKER_PID`（默认值见脚本头注释）。产物落 `results/stage4-sandbox/`：`replay_detail/rounds/summary/paired` CSV + `replay_env/replay_err` 留痕 + `replay_pages/` 页表。
- **状态**：合成数据自测 27/27 通过；待服务器实测出数（跑前确认 DML `LD_LIBRARY_PATH`，见风险 #4）。

### [dsa_bench.cc](dsa_bench.cc) — DSA vs CPU 基准测试（阶段 2）

- **性质**：Google Benchmark 程序，对比三种路径的 memcpy/memset：glibc CPU 基线、DML software、DML hardware（DSA）。
- **依赖**：DML 高层 C++ API（`dml/dml.hpp`）、Google Benchmark。
- **单次 DSA 操作上限 2MB**（受 `setup_dsa.sh` 的 `--max-transfer-size` 限制），size 扫描范围 4KB~2MB。
- **输出**：CSV（real_time / cpu_time / bytes_per_second），用于画加速比曲线、找盈亏平衡尺寸。
- **注意**：同步 `execute` 内部 busy-poll，`cpu_time ≈ real_time`，只能对比吞吐；CPU 释放率需异步 submit 单测（后续 `dsa_cpu_offload.cc`）。
- **编译**：

  ```bash
  g++ -std=c++17 dsa_bench.cc \
    -I/home/m00954035/.local/include \
    -L/home/m00954035/.local/lib \
    -ldml -ldmlhl -lbenchmark -luuid -pthread \
    -o dsa_bench
  ```

## 六、风险 / 待定决策

1. **盈亏平衡已实测 ≈ 1MB**：memcpy 在 1MB 打平、2MB 反超（DSA 27.4 vs CPU 16.4 Gi/s，CPU 跨 NUMA 惩罚），但 4KB 单发 DSA 慢 CPU 78 倍（固定提交开销 ~4.8us）。沙箱 4KB 页操作单发 DSA 失效，必须靠 Batch 聚合成大作业（阶段 3 核心假设）。
2. **Rust FFI 成本**：Firecracker 是 Rust，接 DSA 需 FFI 调 libaccel-config 或走内核接口，阶段 4 前需想清楚。
3. **跨架构外推（软件替代分析的关键）**：DSA 加速比是「Intel 硬件 vs 同平台 CPU」，外推到鲲鹏时 CPU 基线不同（鲲鹏 memcpy ≠ 8480+），需在鲲鹏上跑同样的 CPU/NEON/SVE 软件 baseline 做对照，才能量化「软件能补多少、硬件额外价值多少」——这依赖鲲鹏测试环境。
4. **DML 依赖 libaccel-config**：DML 硬件路径运行时需动态加载 `libaccel-config.so`（缺失会静默失败返回 status=16、数据不搬）。已源码编译 idxd-config 补装到 user-local，运行前需 `export LD_LIBRARY_PATH=/home/m00954035/.local/lib`。
5. **shared 队列实际收益**：shared+ENQCMD 需 SVA/IOMMU（已具备），但并发吞吐是共享提升还是争抢，要实测。

## 七、当前进度

> 以下清单保留历史阶段记录，当前数值与待办以[CURRENT_STATUS.md](CURRENT_STATUS.md)为准：旧942/674与ext4约55%停用；真实pause已有新汇总、原始证据待归档；resume与真实DSA端到端A/B尚未完成。

- ✅ 阶段 0 完成（探测 + 结论已沉淀到本文档）。
- ✅ 阶段 1 完成（DSA 已启用，`/dev/dsa` 节点生成）。
- ✅ 阶段 2 完成（数据搬运部分）：memcpy 吞吐对比已出（盈亏平衡 ~1MB，见上文）；fill/compare/delta/CRC 不在研究范围，已排除。
- ✅ 阶段 3 完成：真异步释放 89.76% + 4KB Batch（N≥48 反超 CPU 5~11 倍），见上文。
- ✅ 阶段 4.1 完成：散落页 batch 物理前提成立 + 页温度（5~11× 仅冷页成立），见上文。
- ✅ 阶段 4.2 完成：AgentENV snapshot 搬运路径坐实（`process_vm_readv` + `pwrite`，942ms/GB）；DSA 卸载 4KB 页有正收益（busy-poll +78.4%，「负收益」是 usleep 假象）；异步等待已源码查证（AgentENV 同步搬运 + io_uring 可用），见上文。
- 🚧 阶段 4.4 A 臂基线：0/64/256MB 已测（wall 含 ~1s 固定等待、CPU ≈ 674 ms/GB，见 4.4；1024/2048 待补）。4.3（Rust FFI）与 4.4 B/C 臂（改源码 A/B）已后移——待 4.5 透明插桩出数后启动。
- 🚧 阶段 4.5 进行中（透明插桩，leader 确认不改 AgentENV 源码）：A 线观测与口径闭合（bpftrace + calibrate 已就绪）、B 线运行态回放已实现（`replay_offload.c` + `replay_run.sh`，合成数据自测 27/27 通过，待服务器实测出数）、C 线干扰实验待做，见 4.5。
- ⏸ 阶段 5（结论报告）待推进。
