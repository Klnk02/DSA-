# 4.5 透明插桩 B 线答疑

> **历史方案说明，2026-10-19修订**：当前结论见[CURRENT_STATUS.md](CURRENT_STATUS.md)。下列原方案中的“同一份源、checksum兜底guest静止、GET无副作用、pvm−DSA即业务释放量”均未获验证，不作为对外结论。anon/nonce的CPU/DSA各自预取本地源、预取窗外；64点checksum不保证跨臂内容一致。原始结果归档、dirty-log语义、源一致性及resume分母均待补，不是只剩64KB粒度校准。

> 对应 leader 遗留三问：① 不改 AgentENV 源码怎么测 pause/resume 的 mem2mem 收益；② 怎么保证测到的确实是 pause/resume 性能变化；③ 统计尺度是什么、为什么。
> 工具：`replay_offload.c`（单进程三臂回放）+ `replay_run.sh`（全自动编排），已完成合成数据自测 27/27，待服务器实测。

## Q1 怎么实现的（零源码改动）

对 AgentENV 的全部接触只有两类：`aenv` 公开 CLI（start / exec / pause / resume，与 A 臂同入口）和操作系统公开接口。链路：

1. 编排脚本自动起沙箱 → guest 内 dd 顺序写制造真实 dirty 页（制脏前先写魔数/nonce/位置编码三个锚点文件，见步骤 2~4）→ **运行态**（`aenv pause` 之前）定位 + probe + 三臂回放
2. 定位 guest RAM，**三路自动择路**：**nonce 内容定位**（共享服务器主路——guest 每轮写 16B 随机数，host 侧在全部 firecracker 进程的可读大段内内容搜索：命中进程 = 本沙箱 fc 真身，命中段所在同文件连续段组 = guest RAM；实测 patch 版后端是 mmap `/dev/ublkbN` 拆两段 VMA，进程名/匿名大小两类匹配均失效，内容定位是唯一可靠手段）；上游 Firecracker 用 memfd 存 guest RAM——`pidfd_getfd`（内核 ≥5.6，root）从 firecracker 进程**借出** fd；均不可用时降级 `/proc/<pid>/maps` 匿名匹配推断基址，再用魔数文件做内容级校验。不注入、不 patch、不链接 AgentENV 二进制
3. 定位后的读法：memfd 模式 mmap 进回放进程只读直映；anon 模式用推断 hva 走 `process_vm_readv`（pvm 臂）或一次性预取进本地缓冲（mmap/dsa 臂，计时窗口外——真实场景 orchestrator 同进程直映直接持有指针，预取成本不计入口径）
4. **只读** KVM dirty log（`KVM_GET_DIRTY_LOG`，绝不 CLEAR，不破坏增量快照链）——运行态（pause 之前）GET，位图尚未被 capture 消费，拿到的是本窗口（自上一快照点以来）真实脏页集合；拿不到时保守退化为全量采样
5. 三臂回放**同一批**脏页：
   - `pvm` 臂 = AgentENV 现状路径（`process_vm_readv` 从 firecracker 读，4.2 perf 已坐实为其实际搬运 syscall）
   - `mmap` 臂 = 单机内存拷贝上界参考
   - `dsa` 臂 = DML batch page 粒度提交（N=48，busy-poll）
6. 每臂64点checksum：pvm/memfd用独立参考读；anon/nonce的CPU/DSA对照本地预取源。只验证抽样搬运，不能证明guest静止或跨臂源相同；不一致rc=2立即中止。
7. 三臂完成后 `aenv pause` → `aenv resume`（保留为快照点：重置下一轮 dirty 基线 + pause/resume wall 与 server CPU 差分逐轮留痕）

**为什么这样设计**：pause会解除相关COW映射，工具选择运行态。dirty-log语义待确认；各臂预取时间和源模式不同，64点checksum不保证guest静止或差异只来自搬运方式。

## Q2 怎么保证是 pause/resume 搬运的性能变化，不是别的

五层防线，每层对应一类质疑：

| 质疑 | 防线 |
|---|---|
| 三臂跑的条件不同？ | 同轮三臂读**同一份页集合文件**（逐轮 dump 留档可审计），唯一变量是搬运方式 |
| 基线是模拟的？ | pvm 臂 syscall 与 AgentENV 现状一致（4.2 perf 坐实），不是假设路径 |
| 数据搬错了也算数？ | 每臂 64 点 checksum 机器验证，错误即中止（rc=2），不靠人眼 |
| 环境漂移/顺序偏差？ | ABBA 交错（每 5 轮换序 + 块内确定性洗牌），预热与缓存效应在臂间对称 |
| 混入其他进程的 CPU？ | 见下 |

**排除其他进程 CPU 时钟**（遗留问题③）：回放进程自身 CPU 用 `CLOCK_PROCESS_CPUTIME_ID`（进程口径，天然不含任何其他进程）；同一沙箱内配对，跨沙箱 40% 方差（单机实测 674 vs 942 ms/GB）被配对差分消掉；NUMA 自动绑定 guest RAM 所在 node；每 rep 前 256MB 冲刷 LLC 统一冷页；governor / host busy% / gcc / 库路径等 20 项环境留痕，事后可核对。

**收益折算未闭环**：516±74 ms/GiB是整体CPU斜率，不能直接乘局部搬运降幅。先解决源访问、计入预取与新增成本，再以真实A/B每事件CPU差结合业务频率估算资源收益。

**当前边界**：B线是局部回放，非snapshot内替换；本地源checksum不保证guest静止。13.85%仅是已识别copy符号占比；resume返回窗口也未覆盖全部后续工作，真实resume分母待测。

## Q3 统计尺度是什么，为什么

- **测量单元**：每轮每臂 5 reps → 先求均值（同页集合，rep 间差异纯为噪声）
- **配对单元**：轮。同沙箱同轮内 `dsa`/`mmap` vs `pvm` 逐轮相减 → 差值序列
- **样本量**：30 轮/档（dirty 64 / 256 / 1024 MB 三档）；**95% CI = 1.96·sd/√n，CI 不跨 0 才下结论**
- **剔除规则预注册**：checksum≠1 行剔除、DSA 臂故障该轮降级剔除（rc=3）、缺臂轮自动配对降级——全部打 WARN 留痕，无主观丢数
- **为什么配对而不是独立采样**：单机实测同配置跨沙箱方差 40%，独立样本会被环境噪声淹没；配对设计把共同环境项相减，只留搬运方式差异
- **为什么 rep 求均值后再跨轮统计**：rep 共享同一页集合与沙箱状态，是重复测量不是独立样本；轮才是独立单元

## 当前状态与下一步

- 两个脚本已就绪（统计输出与手算期望逐字节比对通过）；服务器上 `SMOKE=1` 冒烟 → 全量一键跑完
- 待验证项不止64KB粒度：先补原始证据、完整失败判定、resume基线，再确认dirty-log副作用和跨臂源口径。64KB iovec合并仍需A1实测校准。未过门禁不直接全量运行，详见CURRENT_STATUS.md。
