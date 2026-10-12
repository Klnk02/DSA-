# DSA-：Intel DSA 对沙箱的影响性探索

> 目标：以 Intel DSA 为参照，量化「数据搬移/内存操作硬件卸载」对沙箱（Firecracker microVM）关键路径的实际收益，作为评估「鲲鹏（ARM）是否需要实现 DSA 等价功能」的决策依据。

> 本文档与仓库内脚本同步维护，脚本变更时需一并更新对应章节。

## 〇、背景与定位

- **终极问题**：鲲鹏（Kunpeng/ARM）目前**没有** Intel DSA 这类「内存操作卸载引擎」（memcpy/memfill/compare/delta/CRC 的硬件加速）。要不要补这个能力？补哪些操作？
- **本项目做法**：在 Intel Sapphire Rapids（8480+）机器上实测 DSA 的加速收益，把 DSA 当作「价值标尺」，量化它到底值多少。
- **边界澄清**：鲲鹏 KAE 已具备**加解密 + zlib/gzip 压缩**加速，故压缩不是缺口；本项目聚焦 DSA **独有的**数据搬移/内存操作能力。
- **交付形态**：价值评估报告 + **软件替代分析**（鲲鹏 NEON/SVE 软件能补上多少，作为「要不要造硬件」的必要对照）。

## 一、贯穿全程的三个核心判断

1. **DSA 有盈亏平衡点，不是处处划算**。CPU 在大块连续内存上 memcpy 已经能到几十 GB/s，而 DSA 每次提交有固定开销（构造描述符、写提交、等完成/轮询）。小尺寸下 DSA 反而不如 CPU——基准阶段最重要的任务就是测出这个"盈亏平衡尺寸"。

2. **沙箱值钱的是「并发」和「批处理」，不是单发**。沙箱平台是成千上万 VM，单进程测单次提交的极限吞吐意义有限。真正贴近沙箱的是 shared 队列 + ENQCMD 的并发共享，以及把海量 4KB 页操作打包成 DSA 的 Batch 描述符。

3. **压缩不是本次重点**。DSA 本身不含压缩（压缩在 IAA/QAT），而鲲鹏 KAE 已能做压缩；沙箱内存层压缩用 LZ4/zstd，也与 IAA 的 Deflate 对不上。故本项目聚焦 DSA 独有的「数据搬移/填零/比较/差异/CRC」，压缩留作旁支。

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
| 2 核心基准 | DSA vs CPU 逐操作对比 | 加速比矩阵 | 每个操作、多大尺寸才划算 |
| 3 场景建模 | 把沙箱热点抽象成 DSA 微负载 | 贴近沙箱的 benchmark | 沙箱典型负载 DSA 有没有戏 |
| 4 沙箱集成 | 把 DSA 接进 Firecracker 1-2 条热路径 | 接入 DSA 的沙箱 + 端到端数据 | checkpoint/resume 实际快了没 |
| 5 结论报告 | 汇总 + 软件替代分析 | 报告 + 建议 | 鲲鹏要不要做等价功能、软件能补多少、优先做哪些操作 |

### 阶段 2 基准维度（核心）

- **操作**：memcpy、memfill、compare、delta（差异）、CRC32C、Dualcast。
- **对比轴**：吞吐（GB/s）、延迟（us）、CPU 占用率、批量大小、dedicated vs shared。
- **关键输出**：一张「操作 × 模式 × 尺寸 → 加速比」矩阵 + 盈亏平衡点。
- **双用途**：这组 CPU baseline 同时是「软件替代分析」的参照——ARM 上这些操作只能用 NEON/SVE 软件做，需用同样 benchmark 在鲲鹏上跑出软件基线，与 DSA 硬件值对比，得出「软件能补多少、硬件还有多少额外价值」。

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
- **换算到目标**：vCPU 可用资源提升 = 搬运占比 × 89.76%。若搬运占 vCPU 总负载 6%~11%，则提升 ≈ 5%~10%（搬运占比待阶段 4 实测）。

**4KB 页操作 Batch 打包**（冷数据，16MB 连续 4KB 块，`dsa_batch_4k.c`，数据见 [results/stage3-batch/batch_4k.csv](results/stage3-batch/batch_4k.csv)）：

| batch 大小 N | 吞吐 | vs CPU(7.9 GB/s) |
|---|---|---|
| 4 / 8 / 16 / 32 | 1.6~4.4 GB/s | 0.2~0.6×（慢） |
| **64** | **39.8 GB/s** | **5.0×（阈值）** |
| 128 / 256 | 52.6 / 65.3 GB/s | 6.7× / 8.3× |

- **存在批量效率阈值 N≈64**：N<64 时 DML/DSA 未真正批量（逐个提交，仍慢）；N≥64 才发挥批量效率，吞吐质变（4.4 → 39.8 GB/s）。
- **对沙箱是利好**：单发 4KB 慢 78×，但攒够 ≥64 个 4KB 页操作打包，DSA 反超 CPU 冷拷贝 5~8 倍。沙箱的快照/dirty-page/zeropage 天然是批量页操作，适合攒批。

## 四、沙箱的 DSA 接入点

| 沙箱环节 | 底层操作 | DSA 能力 |
|---|---|---|
| 快照生成「ExportToDiff 拷数据」 | 拷贝 + 差异 | memcpy + delta |
| CoW / dirty page 检测 | 比较 + 差异 | compare + delta |
| guest 内存初始化 / zeropage | 填零 | memfill |
| 快照恢复（磁盘→内存） | 拷贝 | memcpy |
| 零拷贝 / Direct I/O 搬移 | 拷贝 | memcpy |

## 五、脚本清单

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

- ✅ 阶段 0 完成（探测 + 结论已沉淀到本文档）。
- ✅ 阶段 1 完成（DSA 已启用，`/dev/dsa` 节点生成）。
- 🔄 阶段 2 进行中：memcpy/fill 吞吐对比已出（盈亏平衡 ~1MB，见上文）；compare/delta/CRC 待测。
- ✅ 阶段 3 完成：真异步释放 89.76% + 4KB Batch（N≥64 反超 CPU 5~8 倍），见上文。
- ⏸ 阶段 4-5 待推进。
