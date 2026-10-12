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
| 工具链 | accel-config 4.1.8 已装；DML 库/头文件缺失 |

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

## 四、沙箱的 DSA 接入点

| 沙箱环节 | 底层操作 | DSA 能力 |
|---|---|---|
| 快照生成「ExportToDiff 拷数据」 | 拷贝 + 差异 | memcpy + delta |
| CoW / dirty page 检测 | 比较 + 差异 | compare + delta |
| guest 内存初始化 / zeropage | 填零 | memfill |
| 快照恢复（磁盘→内存） | 拷贝 | memcpy |
| 零拷贝 / Direct I/O 搬移 | 拷贝 | memcpy |

## 五、脚本清单

### [check_dsa_env.sh](check_dsa_env.sh) — 环境探测

- **性质**：纯只读，无副作用。
- **探测内容**：系统信息、DSA/IAA 硬件、idxd 驱动、设备与工作队列、accel-config/DML 工具链、IOMMU、大页/NUMA。
- **用法**：`bash check_dsa_env.sh`

### [setup_dsa.sh](setup_dsa.sh) — 启用并配置 DSA

- **性质**：改设备状态（仅 `dsa*`，不碰 IAA 的 `iax*`），需 root。
- **配置**：每颗 DSA 配 1 个 dedicated 队列（`wq{idx}.0`，独占 `engine{idx}.0`）+ 1 个 shared 队列（`wq{idx}.1`，ENQCMD/SVA 共享 `engine{idx}.1`），其中 `{idx}` 为设备编号（`dsa0→0`、`dsa2→2`）。
- **用法**：`sudo bash setup_dsa.sh`（幂等，可重复执行）。
- **两个前置条件（踩过的坑）**：
  1. 内核启动参数**必须去掉 `iommu=pt`**，否则 PASID 挂载失败，user 队列无法启用。正确为 `intel_iommu=on,sm_on`。
  2. user 队列**必须显式 `--driver-name=user`**，否则 `enable-wq` 报 `IDXD_SCMD_WQ_NO_DRV_NAME (0x80200000)`。注意 `driver_name ≠ name`，是两个不同字段。
- **回滚**：

```bash
sudo accel-config disable-wq     dsa0/wq0.0
sudo accel-config disable-wq     dsa0/wq0.1
sudo accel-config disable-wq     dsa2/wq2.0
sudo accel-config disable-wq     dsa2/wq2.1
sudo accel-config disable-device dsa0
sudo accel-config disable-device dsa2
```

## 六、风险 / 待定决策

1. **盈亏平衡未知**：若 DSA 盈亏点在 MB 级，而沙箱热点多是 4KB 页，DSA 单发就失效——必须靠 Batch 聚合成大作业（阶段 3 要验证的核心假设）。
2. **Rust FFI 成本**：Firecracker 是 Rust，接 DSA 需 FFI 调 libaccel-config 或走内核接口，阶段 4 前需想清楚。
3. **跨架构外推（软件替代分析的关键）**：DSA 加速比是「Intel 硬件 vs 同平台 CPU」，外推到鲲鹏时 CPU 基线不同（鲲鹏 memcpy ≠ 8480+），需在鲲鹏上跑同样的 CPU/NEON/SVE 软件 baseline 做对照，才能量化「软件能补多少、硬件额外价值多少」——这依赖鲲鹏测试环境。
4. **DML 缺失**：阶段 2 优先用已具备的 libaccel-config，先不引 DML 依赖。
5. **shared 队列实际收益**：shared+ENQCMD 需 SVA/IOMMU（已具备），但并发吞吐是共享提升还是争抢，要实测。

## 七、当前进度

- ✅ 阶段 0 完成（探测 + 结论已沉淀到本文档）。
- ✅ 阶段 1 完成（DSA 已启用，`/dev/dsa` 节点生成）。
- ⏸ 阶段 2-5 待推进，下一步是核心基准测试。
