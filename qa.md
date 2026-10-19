# qa.md —— B 线回放实验：定位与布局求解方法详解（可信性论证）

> **2026-10-19校正**：当前口径见[CURRENT_STATUS.md](CURRENT_STATUS.md)。本文保留布局探索过程；“三臂同一份内存”“checksum保证guest静止”“GET绝无副作用”“pvm−DSA即可释放CPU”等旧断言停用。anon/nonce分支各臂本地预取、计时不含预取，64点checksum只验证抽样搬运。服务器闭环与dirty-log语义仍待确认。

> 配套文档：[README.md](README.md) §四/§4.5（路线与脚本清单）、[leader_qa.md](leader_qa.md)（leader 三问）。
> 本文回答一个问题：**replay 工具给出的三臂对比数据，凭什么可信？**
> 全文按表述纪律标注证据等级：【实测】= 服务器上观察到的现象；【推演】= 从实测事实出发的逻辑论证；【待实证】= SMOKE 重跑后才能确认的预期；【口径】= 统计口径说明。

---

## 〇、TL;DR

三臂回放（pvm 复刻现状 / mmap 直映 / dsa 硬件搬运）要可比，前提是**三臂读的必须是同一批页、同一份内存**。这依赖三个定位问题全部答对：

| # | 定位问题 | 方法 | 端到端门禁 |
|---|---------|------|-----------|
| 1 | 共享服务器上哪个进程才是目标沙箱的 firecracker？ | guest 写 16B 随机 nonce，host 全机 fc 进程内容搜索 | nonce 本身（2⁻¹²⁸ 碰撞）+ 命中即校验 |
| 2 | 该进程的地址空间里哪一段是 guest RAM？ | nonce 命中段 → 同文件段 → 地址链式连续扩展 | 段组总大小 vs `--guest-mib` 交叉校验 |
| 3 | KVM dirty 位图里的 gpa，如何映射到 hva？ | 段组 **foff 升序拼接**查表 + **memslot 布局实测求解** | **.b0loc 位置编码**（gpa→off→hva→内容全链路校验） |

问题 3 是最险的一环：审计发现旧线性模型在真实目标上**整体错位 512MiB**，且错位数据**三臂自洽、无法靠内部一致性检出**——必须靠外部内容锚点（.b0loc）把关。本文主体就是这三个问题的方法与论证。

---

## 一、方法要解决的三个定位问题

### 1.1 为什么这三个问题都难

**问题 1（进程真身）**：目标机是共享服务器，实测多个 firecracker 进程并存；沙箱的 uuid 在各 fc 的命令行里都能匹配到（或都匹配不到），`find_fc` 的 uuid 匹配失效【实测：曾出现「扫 4 个 firecracker 均未命中」的 nonce-miss，根因之一就是进程枚举与定位不可靠】。选错进程 = 后面所有测量都在测**别人的沙箱**。

**问题 2（RAM 段组）**：上游 firecracker 的 guest RAM 是一条匿名大 VMA（可按「匿名 + 大小匹配」定位）；但目标机的 patch 版 fc 后端是 `mmap /dev/ublkbN`【实测】——**有文件路径、且被拆成多条地址连续的 VMA**，旧的「匿名大小匹配」推断必然失败或选错段。选错段 = 读到的不是 guest 内存（或只是其中一部分）。

**问题 3（位图→hva 映射）**：dirty 位图的每个 bit 对应一个 guest 物理地址（gpa），把它换成 host 虚拟地址（hva）需要知道 **memslot 划分**（slot0/slot1 各覆盖哪段 gpa、文件后端各占哪段 foff）。patch 版 fc 的 memslot 划分与文件布局**都没有文档**，而实测段组的 foff 排布是**逆序**的（见 §3.3）——任何「想当然」的固定假设都可能整体错位。映射错 = 位图说页 A 脏、实际去读页 B，**测量口径整体失真**。

### 1.2 错一环会怎样：自洽但失真（最危险的失败模式）

关键在于：**这三个问题任何一环错了，工具不会报错**。

- 三臂（pvm/mmap/dsa）用的是**同一个** `lin_to_hva` 映射模型：映射错则三臂同错——都去读同一批「错误的页」；
- 每臂结束时的 checksum 校验（`verify_dst`，64 点均匀抽样 FNV 对比）用的也是**同一个**映射：错的映射读参考、错的映射搬数据，**checksum 照样一致**；
- 输出的 CPU ms/GB 数字完全正常，图表漂亮，结论全错。

这是「错位数据」的典型特征：**内部自洽、口径失真**。靠工具内部任何一致性检查都检不出来，必须引入**外部内容锚点**（见 §3.5 .b0loc 门禁）。这个失败模式不是假想的——§3.3 记录了旧模型 512MiB 错位的实测发现过程。

---

## 二、端到端流程（一轮 round 的时序）

`replay_run.sh` 每轮（每个 dirty 档位 × round）按以下时序执行，C 工具（`replay_offload`）与脚本各司其职：

```
┌─ guest 侧（aenv exec，沙箱运行中）────────────────────────────┐
│ ① 写哨兵三件套（各带 stat 自检，失败打 WARN 不中止）：          │
│    .b0magic  64KiB 固定魔数（anon 降级模式的基址校验锚）        │
│    .b0nonce  16B /dev/urandom 随机数，od 读回 → NONCE_HEX       │
│    .b0loc    16 页 × 4KiB 位置编码（布局求解端到端门禁锚）      │
│ ② 制脏：dd 写 tmpfs 指定量（DIRTY_MB_LIST 档位）               │
└──────────────────────────────────────────────────────────────┘
┌─ host 侧（运行态，fc 的 guest RAM = rw-p COW 页，可读）────────┐
│ ③ probe（replay_offload --arm probe）：                        │
│    B0 定位：nonce 内容搜索 → fc 真身 + RAM 段组（§3.1/§3.2）   │
│    B1 位图：只读 GET slot0/1（严禁 CLEAR）→ 布局求解（§3.4）   │
│             → .b0loc 端到端校验（§3.5）→ dump 页集合文件        │
│             （文件尾带 #B0ORDER 模型行，传给臂进程 §3.6）       │
│    输出：B0=GO ... loc=14/16 s0=3072MiB + DIRTYSRC=bitmap ...  │
│ ④ 三臂回放（同一 dump 文件，逐臂串行）：                       │
│    pvm：process_vm_readv 跨进程读（复刻 AgentENV 现状路径）     │
│    mmap：本地预取缓冲 memcpy（直映上界参照）                    │
│    dsa：DML 硬件搬运（若 rc=3 则本轮剔除 dsa 继续）             │
│    每臂 verify_dst 64 点 checksum，rc=2 不一致 → 全脚本中止     │
└──────────────────────────────────────────────────────────────┘
          ↓ aenv pause（留 wall）→ aenv resume → 下一轮
```

流程要点：

- **哨兵先于制脏写入**：nonce/.b0loc 也是脏页，会被位图捕捉，probe 正好拿它们做校验锚——制脏量里天然包含哨兵页（口径影响 ≤ 128KiB，可忽略）【口径】。
- **三臂共用同一 dump 文件**：臂进程各自重新做 nonce 定位（运行态 fc 的 COW 页稳定在场，地址空间不变），排列模型由 dump 尾 `#B0ORDER` 行统一（§3.6）——保证「同一批页、同一映射」。
- **失败快停**：probe NOGO / 缺 DIRTYSRC / checksum 不一致都直接 fatal，不产出坏数据；dsa 臂硬件失败（rc=3）降级剔除本轮 dsa 数据点。

---

## 三、逐层方法与论证

### 3.1 问题一：nonce 内容定位进程真身

**方法**（`find_by_nonce` → `list_fc_pids` + `nonce_scan_pid`，replay_offload.c ≈L462/L495/L633）：

1. **双口径枚举全机 firecracker 进程**：`/proc/*/comm == "firecracker"` **或** `/proc/*/cmdline` 含 firecracker 字样（`list_fc_pids`）。双口径是修复教训：曾只用 comm 精确匹配漏掉目标 fc【实测】。自身进程排除（cmdline 会误含参数字样）。
2. **逐进程内容搜索**：对每个候选 pid，遍历 `/proc/pid/maps` 的 **可读且 ≥16MiB** 段（guest RAM 量级过滤，跳过栈/堆等小段；**只要求可读不要求 rw**——运行态 guest RAM 是 rw-p COW 页，实测 aenv pause 会把该段 flush+unmap（fc 全 maps 最大段只剩 3MiB r-xp）导致可读 ≥16MiB 段 0 个、nonce 全 miss【实测】——这正是三臂回放必须改到运行态（pause 之前）的根因；工具对 fc 内存只读不写（pread / pvm 读）。miss 且无候选段时自动打印该 pid 全 maps 最大段及权限——rw-p 段消失 = pause 已 unmap、`<16MiB` → 拆小段需降阈值，根因一眼可辨），`pread(/proc/pid/mem)` 按 **1MiB 块** 读 + `memmem` 搜 16B nonce；**块间回退 15B 重叠**，防 16B nonce 恰跨块边界漏检。每 pid 打印诊断明细（段数/实读字节/pread 失败数/首个 errno）。
3. **命中即裁定**：唯一命中 nonce 的 pid = 目标 fc。若调用方传了 `--fc-pid` 且与命中不一致，以 nonce 为准并打 WARN（uuid 匹配在共享服务器失效，见 §1.1）。

**可信性论证**：

- **碰撞概率**：nonce 是 16B 均匀随机数（每轮重新生成、od 读回 32 hex 交叉确认）。别的进程内存里天然含这段字节的概率 ≈ 2⁻¹²⁸ × 全机内存字节数 ≈ 10⁻³¹ 量级——实际上不可能【推演】。即 nonce 命中 ⟺ 该进程的内存里**有 guest 刚写的这轮随机数** ⟺ 它就是本轮制脏的沙箱真身。
- **假阴性防御**（漏检真身）：16B 跨块边界漏检已由 15B 重叠封死；`/proc/pid/mem` 打不开的进程（别家 fc 并发启停、竞态退出）按「不可读跳过」处理并计数——若**全部**不可读，报错信息直接指向权限/yama（要求 root 运行）。【待实证：SMOKE 重跑确认双口径枚举在真实多 fc 环境命中】
- **为什么不用 pidfd/uuid**：uuid 匹配失效是实测事实；pid 可能复用。内容定位是唯一不依赖任何外部命名约定的方式，且它顺带解决了问题 2（见下）。

### 3.2 问题二：同文件链式连续段组 = guest RAM

**方法**（`collect_ram_segs`，≈L556）：nonce 命中段（起始 hva = hit_s）已知是 guest RAM 的一部分；沿 `/proc/pid/maps`（天然按地址升序）收集**同文件**（路径字符串相同；空 = 匿名组）的可读 ≥1MiB 段（同 §3.1：运行态 guest RAM 是 rw-p COW 页，只读即可），再从命中段向两边做**地址链式连续扩展**（前段尾 == 后段头），还原完整段组。

**可信性论证**：

- **不漏**：真实 guest RAM 常被按权限/映射时机拆成多条地址连续 VMA【实测：ublk 后端拆两段】；链式扩展把同文件且地址无缝相邻的段全部收回。段与段之间若有地址空洞（非 RAM 的其他映射插入），扩展自然停止——不会把无关映射误并入。
- **不多**：段组全部来自**同一文件**（`/dev/ublkbN`）。该设备文件只被 fc 用于 guest RAM 后端；同名文件的其他用途映射（若有）也会因地址不连续而被链式扩展隔离。
- **交叉校验**：段组总大小 vs `--guest-mib`（用户传参）不一致时打 WARN 并**以实测为准**【实测：patch 版后端非匿名大 VMA，正是旧「匿名大小匹配」推断失败的原因】。
- **边界情况**：匿名组（无文件名）时 foff 全 0，无法按 foff 排序——此时排列候选交给 §3.4 的双排列枚举 + .b0loc 裁定。

### 3.3 问题三（上）：foff 升序拼接映射模型，与 512MiB 错位的由来

**先定义线性偏移空间**。KVM dirty 位图按 memslot 分片：slot0 的 bit i 对应 gpa `i*4K`，slot1 的 bit j 对应 gpa `4G + j*4K`。x86_64 有 [3.5G, 4G) 的 MMIO hole，guest RAM = [0, s0) ∪ [4G, 4G+ram−s0)（s0 = slot0 字节数）。工具把两段**挤掉 hole 顺序拼接**成「线性偏移空间」[0, ram)：

- slot0：bit i → 线性偏移 `i*4K`（= gpa）
- slot1：bit j → 线性偏移 `s0 + j*4K`

三臂回放、dump 文件、采样，全都工作在这个线性偏移空间上；`lin_to_hva(off)` 负责最后一步：线性偏移 → hva。

**旧模型为什么错**。旧实现是 `lin_to_hva(off) = base + off`，隐含假设「hva 升序 == 线性偏移升序」。但实测段组 foff 排布是**逆序**的【实测，见下方证据】——低地址段（hva 小）512MiB 的 foff 是 0xC0000000（3GiB），高地址段（hva 大）3GiB 的 foff 是 0。也就是说 hva 序与文件偏移序**相反**，`base + off` 把两段的内容整体对调，边界处等效错位 512MiB。

**实测证据**（probe 的段明细打印，`[b0] 段i hva=[..] size=..MiB foff=..` 行）：

```
段0 hva=[低] size=512MiB  foff=0xC0000000   ← hva 低的段，文件偏移反而在后面
段1 hva=[高] size=3072MiB foff=0x0          ← hva 高的段，文件偏移反而在前面
段组总大小 = 3584MiB = 3.5GiB
```

**危害链**【推演，基于上述实测】：

1. guest 内 tmpfs 制脏页由 guest 内核 GFP_HIGHUSER_MOVABLE 分配，偏好 guest ZONE_NORMAL（gpa ≥ 4G，即 hole 之上的 slot1 区间）——制脏页大概率落在 slot1 的位图里；
2. 旧模型下 slot1 的页要么因换算越界被丢弃、要么被映射到错误 hva——**位图里有、回放集合里没有（或读错页）**；
3. 更糟的是魔数全空间扫（`scan_magic_full`）会「兜底成功」找到魔数——因为它扫的是整个线性空间、不依赖页集合正确性——掩盖错位，制造「基址定位正确」的**误报**；
4. 三臂 + verify 用同一错误模型，数据自洽（§1.2），事后无法从数据本身发现。

**修复：foff 升序拼接查表**（`lin_to_hva`，≈L892）。段组按 foff 升序排列后，令 C_i = 前 i 段字节累计和，段 i 覆盖线性偏移 [C_i, C_i+size)，则 `hva = seg[i].s + (off − C_i)`。

**布局无关性证明**【推演】：文件后端有两种可能的布局，都无法靠 /proc/maps 推理分辨（maps 只给段的 foff 起点，不给「文件里 slot 边界在哪」）：

- **L1（双 slot 顺序布局）**：slot0=[0,3G) 的文件区间 [0,3G)，slot1=[4G,4.5G) 的文件区间 [3G,3.5G)。则文件 foff 升序 = slot 顺序 = 线性偏移序（slot0 bit i → foff i·4K → 线性偏移 i·4K；slot1 bit j → foff 3G+j·4K → 线性偏移 s0+j·4K，其中 s0=3G）。
- **L3（单 slot gpa 直存）**：单 slot [0,3.5G)，文件偏移 = gpa 本身。则 foff 升序 = gpa 升序 = 线性偏移序（s0=3.5G=总 RAM）。

**两种布局下 foff 升序拼接给出完全相同的「线性偏移→hva」映射**（本例都是：线性 [0,3G) → hva 高段、[3G,3.5G) → hva 低段）。所以映射模型不需要分辨 L1/L3——这是 foff 升序拼接相对一切「猜 hva 序」方案的本质优势：**它只依赖「文件以线性偏移序存储 guest RAM」这一条无论哪种布局都成立的性质**。L1/L3 的分辨留给布局求解器（§3.4），那只影响位图怎么解读，不影响映射表。

### 3.4 问题三（下）：memslot 布局求解器——s0 不猜，实测

**未知量**：s0（slot0 字节数）。它决定 slot0/slot1 位图各有多少有效位、slot1 的 bit j 换算成线性偏移时的基址。上面说了 L1 下 s0=3G、L3 下 s0=3.5G——**推理无法分辨，必须实测**。

**方法**（`probe_dirty_bitmap_nonce`，≈L1173）：

1. **位图快照**：`memset(g_bm)` → `KVM_GET_DIRTY_LOG(slot0)` → memcpy 到 bm0；再 memset → GET slot1 → bm1。**每次 GET 前 memset 是硬要求**：KVM 只写该 slot 真实页数对应的字数、不清零缓冲，不 memset 会把上一 slot 残留位当本 slot 的（幻影页）。**只 GET 严禁 CLEAR**——CLEAR 会破坏 AgentENV 增量快照链（本实验的铁律）。errno 在 GET 返回后**立即**取值（后续 slot2/3 探测会覆盖它）。
2. **防御探测** slot2/3：若也有位图（非预期大布局）打 WARN，口径可能不完整。
3. **候选 s0 枚举**（去重）：`{ 总 RAM（单 slot）} ∪ { X86_HOLE_END=3.5G（若 < 总 RAM）} ∪ { 段组 foff 序累计边界 }`。本例 3.5GiB 段组 → 候选 = **{3.5G（L3）, 3G（L1：foff 序第一段 3GiB 的累计边界）}**。
4. **存在性过滤**（候选与 slot 实存一致性）：`s0 == 总 RAM` 但 slot1 GET 成功 → 矛盾，剔除；`s0 < 总 RAM` 但 slot1 GET EINVAL → 矛盾，剔除。即 **slot1 的存在与否本身就把 L1/L3 分开了**。
5. **逐候选重建 + .b0loc 校验**（§3.5）：对每个（过滤后的候选 × 匿名组时正/逆双排列）组合，用 `collect_slot_pages_ex` 从 bm0/bm1 重建线性偏移集合，跑 .b0loc 校验，≥12/16 命中的候选胜出。
6. **胜者固化**：`g_slot0_bytes = s0`，脏页计数按该候选的 slot 页数截断（`bits_of`），页集合 memcpy 输出。**全灭 → NOGO loc-miss**（先跑 `b0loc_scan_full` 全空间扫定位根因，见 §3.5），宁失败不出错数据。

**算术自检**（为什么候选集是对的）【推演】：

- L1 成立时（slot1 存在）：过滤后只剩 s0=3G；slot1 位图有效页数 = (3.5G−3G)/4K = 128K 页，恰是 slot1 真实覆盖的 gpa [4G,4.5G) 的页数——位图解读不重不漏。
- L3 成立时（slot1 EINVAL）：过滤后只剩 s0=3.5G=总 RAM，slot0 位图覆盖全部 896K 页。
- 若真实布局是候选集外的意外形态：没有候选能过 .b0loc 门禁 → NOGO loc-miss，不会静默出错数据。候选集覆盖了「文件后端 memslot 布局」的所有自然形态（单 slot / hole 边界双 slot / 对齐段边界的双 slot）。

**位图不可用时的退化**：位图全空（AgentENV capture 已把日志消费掉 / ring 模式）→ 退化 **whole 口径**（整段 RAM 采样）。此时 foff 序拼接天然不重不漏、三臂读的是同一全页集合，口径本身与排列无关；但布局求解不再有校验素材，GO 行如实标 `loc=n/a`（§3.5 详述）。

### 3.5 .b0loc 位置编码：gpa→hva 端到端门禁（本方法的信任根）

这是整个方法里**唯一能检出 §1.2「自洽但失真」失败模式**的机制，值得完整论证。

**设计**（写入侧：replay_run.sh ≈L367；校验侧：`b0loc_parse16`/`b0loc_check`，≈L966/L988）：

- guest 每轮（制脏前）写 `/dev/shm/.b0loc`：16 页 × 4KiB，**每页页首 16B = `"B0LOC" + 8 位十进制页号(00000000..00000015) + "END"`，其余 4080B 零填充**。即每个物理页自带「我是第几页」的位置编码。
- probe 布局求解时，对每个候选模型：把 dirty 位图位 → 线性偏移 → `lin_to_hva` → `pread` 读页首 16B → 格式校验 + 页号解析，统计 **distinct 页号命中数**；≥12/16（`B0LOC_MIN_HIT`）判该候选正确。

**校验力论证——它到底校验了什么**：一个 .b0loc 命中意味着「位图说某 gpa 脏 → 按 slot 语义换算成线性偏移（考验 s0）→ 按段组模型映射成 hva（考验排列/拼接）→ pread 读到的那 16B 恰好是合法编码且页号在 0..15」——**gpa→off→hva→内容的全链路同时正确**。而 .b0loc 页本身在 dirty 集合里（哨兵先于制脏写入、guest 无 swap 时 tmpfs 页驻留），所以正确模型下 16 页应全部命中（门限 12 留了 4 页余量给极端情况：编码页被 guest 回收/换出、跨段边界拆分等）。

**误放行概率**（错位候选碰巧 ≥12 命中）【推演】：

- 错位候选下，某 dirty 页的映射落点要命中 16 个编码页首之一：落点若均匀分布，单页概率 ≈ 16 页/896K 页 ≈ 1.8×10⁻⁵；即使按最宽松的「落点必在 dirty 页内」口径，也 ≤ 16/n_dirty。
- 且命中还要求落点页首 16B 通过格式校验——随机数据恰好是 `"B0LOC"+8数字+"END"` 的概率 ≈ 10⁻³¹。
- 命中还要求 **distinct 页号**（位图去重），12 个不同编码页各自独立命中。
- 联合概率：即使 128MB 制脏（32K dirty 页、单页上界 1.8×10⁻⁵，期望命中数 0.6），P(≥12 distinct) ≈ 10⁻¹⁶ 量级；制脏更小时更小。**门限 12 在实践中等价于「不可能误放行」**。
- 更本质的一层：本例文件组下候选排列只有 foff 一种（正确），错位只能来自 s0；s0 错 512MiB 时编码页的映射落点整体平移 512MiB——16 页全部错过 64KiB 的编码簇。

**误拦截（假阴性）与根因二分**：全候选 <12 命中 → NOGO loc-miss 前，先跑 `b0loc_scan_full`（1MiB 块流式扫 + 4K 边界页首校验，覆盖整个线性空间）：

- 全扫也 0 命中 → `.b0loc` 根本没写进 guest RAM（aenv exec 失败被吞 / guest 工具链不支持 printf·head / 被回收）→ 查脚本 WARN 行；
- 全扫有命中但所有候选错位 → 非预期 memslot/文件布局 → 报错信息直接指引把 `/proc/<pid>/maps` 与候选诊断行发维护者。

注意全空间扫本身**没有**映射校验力（内容总能扫到，它只回答「在不在」）——所以它只做诊断，不做放行。这与旧代码里 `scan_magic_full` 兜底掩盖错位（§3.3）是同一教训的应用：**内容搜索可以定位「在哪」，不能证明「映射对」**。

**为什么校验用 dirty 全集而非 subset**：.b0loc 页在 dirty 集合里，但 subset 采样可能恰好不采中它们；校验在采样前、对全量集合做，保证编码页一定参与校验。

**whole 口径下 loc=n/a 的合理性**：位图全空 → 全页集合。此时任何排列下「全页集合」都覆盖全部 hva——编码页总能被读到，校验失去区分力（任意排列都「命中」）。但 whole 口径本身与排列无关：三臂拿同一份全页集合、同一映射模型回放，测的是「整段 RAM 的搬运成本」，不是「dirty 页集合的搬运成本」【口径】。GO 行如实标 `loc=n/a`（不带 s0），不冒充有校验。

### 3.6 模型传递与三臂一致性：#B0ORDER 与不变式

probe 求解出的模型（段组排列）必须原样传给三个臂进程，否则臂间映射不一致，三臂对比失去意义。

**传递链**：

```
probe 求解（g_rsegs 排列定型）→ dump 文件尾写 "#B0ORDER foff|asc|desc"
  → run_arm 传 --pages dump 文件 → 臂进程 load_pages 解析到 g_dump_order
  → do_arm 调 set_seg_order(g_dump_order) 重排 g_rsegs
  → prefetch/pvm/verify 全部在重排后的模型上执行
```

**为什么臂进程不能各自求解**：文件组的 foff 序是确定性的（臂本地求解 == probe 结果），但**匿名组**臂本地默认 ASC 排列，而 probe 实测求解可能是 DESC——臂间若各自为政，pvm 用 ASC、mmap 用 DESC，三臂读的就不是同一批 hva。dump 尾行把 probe 的裁定结果作为**唯一真源**下发给所有臂。

**不变式设计**（这是实施中差点引入缺陷、后来修正的点）：`g_b0_order` 恒等于 `g_rsegs` 的当前实际排列。`set_seg_order` 有幂等检查（`order == g_b0_order` 则直接 return）。若 `load_pages` 解析 #B0ORDER 时直接改 `g_b0_order`，会破坏不变式——之后 `set_seg_order(g_dump_order)` 发现「order 相同」误判已就位、**跳过实际重排**，匿名组 desc 场景臂进程静默用错排列。修复：load_pages 只写独立变量 `g_dump_order`（不碰 `g_b0_order`），由 `do_arm` 显式调 `set_seg_order` 应用——幂等检查的语义恢复正确（同值 = 真的已就位）。

**臂进程 nonce 失败不会静默降级**：臂的 `discover_and_setup` 里 nonce 给定但 `find_by_nonce` 失败 → 直接返回失败 → 脚本 fatal。不存在「probe 用段组模型、臂降级到 anonmap 旧模型」的错配路径【推演，代码路径核对】。nonce 完全不可用（生成失败）时，probe 与臂**都**退回 --fc-pid + anonmap 降级模式，dump 无 #B0ORDER 行、无重排——两侧一致地用旧模型（该模式基址已被魔数校验锚定，且不涉足本文的段组布局问题）。

---

## 四、多层防线总表

| 层 | 机制 | 防什么 | 失败表现 |
|---|------|--------|---------|
| 1 | 哨兵 stat 自检 ×3（magic/nonce/loc） | guest 写入没落地（aenv exec 失败、工具链缺失） | 脚本 WARN 行（.b0loc 异常时预告 probe 将 NOGO loc-miss） |
| 2 | nonce 生成后 od 读回 32 hex | 随机数没写进去/读不回来 | WARN + 退回 --fc-pid 定位 |
| 3 | nonce 内容命中（2⁻¹²⁸） | 定位错进程 | 无命中 → NOGO nonce-miss（per-pid 诊断明细） |
| 4 | 段组总大小 vs --guest-mib 交叉校验 | 段组收集错 | WARN（以实测为准） |
| 5 | **.b0loc 端到端门禁（≥12/16）** | **映射/布局错位（自洽失真）** | **NOGO loc-miss**（全扫根因二分） |
| 6 | 魔数校验（.b0magic，anon 全套） | RAM 基址错（降级模式主用；nonce 路双保险） | NOGO magic-miss |
| 7 | verify_dst 64 点 checksum（参考读与被测路径机制独立） | 搬运路径的数据损坏 | rc=2 → 全脚本立即中止 |
| 8 | 编排层：probe NOGO/缺 DIRTYSRC fatal；dsa rc=3 剔除 | 上游坏数据流入汇总 | fatal / WARN + 剔除 |

注意层次关系：第 5 层是唯一能拦「自洽但失真」的（§1.2）；第 7 层只能拦「搬运过程中损坏」，拦不住「搬错了页」。

## 五、失败诊断：NOGO / WARN 判读表

| 信号 | 含义 | 下一步 |
|------|------|--------|
| `B0=NOGO reason=nonce-no-fc` | 全机无 firecracker 进程 | 沙箱没起？aenv status 检查 |
| `B0=NOGO reason=nonce-miss` | 扫遍 fc 进程未命中 nonce | 看 `[b0] nonce-scan pid=..` 明细行：实读 0MiB → 权限问题（用 root）；段 0 个 → maps 解析问题；都正常 → nonce 没写进 guest（查脚本 WARN 行） |
| `B0=NOGO reason=nonce-seg` | 命中了但段组收集失败（maps 已变） | 重跑；持续出现则 fc 地址空间在扫描间隙变动，找维护者 |
| `B0=NOGO reason=loc-miss` + 全扫 0 命中 | .b0loc 没写进 guest RAM | 查脚本 `.b0loc 大小异常` WARN；guest 工具链（printf/head）是否可用 |
| `B0=NOGO reason=loc-miss` + 全扫有命中 | 非预期 memslot/文件布局，候选集没覆盖 | 把 `/proc/<pid>/maps` + `[b1] 布局候选` 诊断行 + 段明细发维护者 |
| `B0=NOGO reason=magic-miss` | 魔数校验失败（RAM 基址错） | 对照 /proc/maps 人工核对（nonce 路下此 NOGO 多为哨兵缺失，已被 loc 门禁先行拦截，此处主要适用于 anonmap 降级模式） |
| GO 行 `loc=n/a` | 位图全空，whole 退化口径 | 不是错误：数据仍有效，但口径是「整段 RAM」而非「dirty 页集合」，汇总时按 src 字段区分 |
| `WARN: slot%d 也有 dirty 位图` | 非预期大 slot 布局 | 口径可能不完整，结果仅作参考 |
| checksum rc=2 | 搬运数据损坏 | 脚本已中止，查 ERRLOG；多为 DML/驱动问题 |
| dsa 臂 rc=3 | DML 硬件失败 | 本轮 dsa 数据点已自动剔除，DSA_FAILS 计数 |

**GO 行新字段判读**（`B0=GO ram_src=nonce ... segs=2 order=foff file=/dev/ublkb0 kvmvm=1 magic=hit@0x.. loc=14/16 s0=3072MiB`）：

- `segs`：段组段数（实测 2）；`order`：求解出的排列（文件组应为 foff，匿名组 asc/desc 之一）；`file`：后端文件（空 = 匿名组）
- `loc`：.b0loc 命中 x/16（期望 16/16，≥12 过门）；**`n/a` = whole 退化**（无 s0 字段）
- `s0`：布局解（slot0 字节数）——L1 预期 3072MiB、L3 预期 3584MiB【待实证】
- `DIRTYSRC=bitmap pages=N mb=X`：页集合来源与规模（bitmap = dirty 口径；whole = 整段口径）

## 六、证据分级

**【实测】**（服务器观察，可信度最高）：

1. 共享服务器多 firecracker 并存、uuid 匹配失效（曾 4 fc 并存且定位失败）
2. patch 版 fc 的 guest RAM 后端 = `mmap /dev/ublkbN`：有文件路径、拆两段 VMA（非上游的匿名单 VMA）
3. 段组 foff 逆序排布：hva 低段 512MiB@foff 0xC0000000、hva 高段 3GiB@foff 0，总 3.5GiB——**这是推翻旧线性模型的直接证据**
4. `/proc/<pid>/mem` 可读（曾成功扫过 4 个 fc 进程的大段内存）

**【推演】**（从实测事实出发的论证，逻辑上封闭）：

1. L1/L3 两种布局假设（双 slot 顺序布局 / 单 slot gpa 直存）与 foff 升序拼接的布局无关性（§3.3 证明）
2. 旧模型 512MiB 错位的机制与危害链（§3.3）
3. 错位数据自洽不可检、需外部锚点（§1.2）
4. .b0loc 误放行概率天文小（§3.5）
5. nonce 碰撞概率 2⁻¹²⁸（§3.1）

**【待实证】**（SMOKE 重跑后确认，当前按设计预期）：

1. slot1 存在性（GET ok vs EINVAL）→ L1/L3 最终裁定
2. .b0loc 命中数（期望 16/16，门限 12）与胜出候选 s0 落点（3G 或 3.5G）
3. bitmap 口径 pages 数与制脏量的对应关系（note 里 `s0:/s1:` 脏页分布——预期制脏页集中在 slot1，即 guest ZONE_NORMAL 偏好成立）
4. 双口径 fc 枚举在真实多 fc 环境下的命中（修复 nonce-miss 的验证）

## 七、已知边界与不覆盖项

1. **只解 slot0/1**：slot2/3 有位图只打 WARN 不解读（x86_64 3.5G RAM 的自然形态用不到；意外大布局会显式 WARN，不静默）。
2. **whole 口径无映射校验力**：位图全空时 loc=n/a（§3.5）——数据仍有效，但口径是整段 RAM；bitmap/whole 两种口径的结果不直接可比，汇总按 DIRTYSRC 区分。
3. **guest ≥ 3584MiB 时 hole 挤压假设生效**：x86_64 常规布局；若目标机用了非常规 GPA 布局，候选集会打尽 → loc-miss 显式失败而非出错数据。
4. **运行态页内容稳定未被保证**。anon/nonce的CPU/DSA臂各自预取guest至本地源，64点checksum对照本地源，只检查抽样搬运，不保证guest未变、跨臂源内容一致或全页正确。pvm/memfd分支的独立参考读仍有抽样和时间差边界。
5. **.b0loc 页若被 guest 回收**（无 swap 下 tmpfs 驻留，概率极低）：命中数下降，<12 时按 loc-miss 失败（宁可失败）。门限 12/16 的 4 页余量覆盖此情况。
6. **dirty-log语义待确认**：不显式CLEAR不能推出GET无副作用。必须确认目标KVM能力、模式、GET是否消费/清位及AgentENV消费时序；确认前不保证页集合的增量区间，不在共享业务实例probe。
7. **本方法测的是「搬运路径的 CPU 成本」，不是端到端 pause 时长**：A 线（bpftrace 归因）负责后者；两线结论互补，不互相替代。

## 八、Q&A：对方法本身的质疑与回应

**Q1：为什么不直接用 AgentENV 的快照数据/接口拿 dirty 页，要在 KVM 侧偷？**
A：约束是不改AgentENV源码；GET是否消费/清位依赖目标配置。定位正确与dirty-log无副作用是两项独立门禁，确认前不宣称完全无感。

**Q2：nonce 找到的进程凭什么就是「目标沙箱」而不是同机别的沙箱？**
A：nonce 是本轮、这个沙箱、guest 内核刚写入 tmpfs 的 16B 均匀随机数（写后 od 读回交叉确认）。别的沙箱内存里出现这 16B 的概率 2⁻¹²⁸（§3.1）。反向风险（漏掉真身）由块重叠扫描 + per-pid 诊断 + 「全不可读」单独报错覆盖。

**Q3：段组会不会多收一段（别的映射）或漏收一段（RAM）？**
A：多收需要「同文件 + 地址无缝相邻」同时成立——别的映射恰好同文件且紧贴 RAM 段，且被 nonce 命中证明其内容确是 guest 内存；漏收则总大小对不上 --guest-mib（WARN 交叉校验）。两方向都有信号。

**Q4：foff 升序拼接凭什么对？万一文件布局两种假设都不成立呢？**
A：§3.3 的布局无关性证明覆盖了两种自然布局；若都不成立（第三种意外形态），.b0loc 门禁全灭 → 显式 NOGO loc-miss，不出错数据。「证明对」和「错了能知道」双保险——后者才是工程上可信的保证。

**Q5：12/16 这个门限怎么定的？会不会放行错位候选？**
A：§3.5 推演：错位候选期望命中数 ≪ 1，P(≥12 distinct) ≈ 10⁻¹⁶ 量级以下；门限留 4 页余量防「编码页被回收」的假阴性。门限在「误放行天文小」与「误拦截极小」之间，偏向宁可失败。

**Q6：三臂怎么保证读的是「同一批页」？**
A：dump与#B0ORDER约束页索引和布局，不保证各臂不同时刻读到相同内容。外部锚点与64点checksum都有抽样边界，不能排除全部静默错配。

**Q7：GET_DIRTY_LOG 会不会扰动被测系统？**
A：尚不能保证。只GET不CLEAR不是无副作用证明；先核对目标配置，在专用测试实例确认AgentENV后续增量快照未丢页，才能运行回放。

**Q8：这套方法的结论边界？测出来的「DSA 可省 X% CPU」怎么用？**
A：按源模式分别解释。相同本地源下CPU与DSA差值是局部搬运成本差；pvm实时读取与窗外预取DSA不能直接相减称为可释放CPU。接入可行、计入新增搬运与同步且完成真实A/B后，才能换算业务收益。回放不是已证明的全局上限。

**Q9：mem2disk / disk2mem 能不能用真实 snapshot 穿刺（在真实路径上直接跑 DSA）来测？**
A：当前DML、当前路径与不改源码约束下尚未实现透明替换；下列为当前限制，不能外推DSA硬件普遍不可能：
1) 真实路径两个方向的搬运都在内核里完成——mem2disk：orchestrator 用 `process_vm_readv` 从 firecracker 地址空间读（内核 `copy_page_to_iter`）再 `pwrite` 进 commit 文件（内核 `copy_page_from_iter_atomic`）；disk2mem：ublk/overlaybd 从 layer 读出、映射缺页把页填回 guest。DML/DSA 只能搬**提交者自己进程地址空间内**的内存，跨进程地址空间、page cache、块层的搬运它够不到（README §4.2 源码级确认）。
2) 「先经内核拷一遍，再交给 DSA 搬第二遍」不省 CPU：数据被搬两次，而第一次仍计在被测进程的 CPU 时间里。这正是 LD_PRELOAD／ptrace 两条路线被排除的同一个理由——拦截得到也改不动，改了也白改（README §4.5 已排除项）。
3) 穿刺能做的只有**观测级**：不改源码、不重启共享守护进程，用 perf/bpftrace 包住真实 pause/resume 窗口取真实 CPU 与符号构成（[bpftrace_pause_attribution.bt](bpftrace_pause_attribution.bt) + [baseline_pause_sweep.sh](baseline_pause_sweep.sh) + [calibrate_cpu_scope.sh](calibrate_cpu_scope.sh)）。观测提升的是**结论可信度**，不改变「DSA 可行域」。

**Q10【历史回答，停用】：旧版收益描述（以CURRENT_STATUS.md为准；下列0收益、上限及I/O证明等强断言不再引用）**
A：三层分开写，不许混：
- **真实基线（观测级，可信度最高）**：mem2disk 方向 = A 段差分回归 **516 ± 74 ms/GiB + 固定 108.3 ms**（0/64/256/1024MB × 20 轮共 80 点，R²=0.708，0/80 失败，2026-10-19 干净全量，同沙箱配对）；disk2mem 方向 = `MODE=resume` 扫描的恢复窗口（待跑；被测集合 = server + fc + uvm-ublk-daemon，缺一即口径下界，脚本会 WARN 并如实标注）。方向性证据（同轮 perf -F4000 包 60 成功事件，87024 样本，符号 100% 解析）：真正的数据搬运 = 读侧 `_copy_to_iter`（process_vm_readv 跨进程 copy_to_user）**11.66%** + 写侧 `copy_page_from_iter_atomic`（pwrite → page cache）**2.20%** = **13.85%**（Wilson 95%CI [13.64, 14.07]）= 71.5 ms/GiB，是单项最大已识别头；其余大项 = page cache/XArray 10.65%、memcg/LRU 8.24%、页表/页管理 4.65%、锁/RCU 3.37%——全部在内核簿记侧，不在 DSA 能力范围。（旧「ext4 簿记 ~55%」为磁盘满污染轮的失败事件重试路径结论，**已作废**：污染轮失败事件 5651 样本/事件 vs 干净轮 1450，重试烧 ~4 倍 CPU。）
- **结构判定**：当前用户态DML没有已验证的直接接管接口。新增staging是候选方案，不代表唯一方案；不能据此断言DSA硬件全局收益为零。
- **上限微基准（已出数，2026-10-19 服务器 60MB×15 冷页 `both`）**：`dsa_disk_scene` 公平三结构——mem2disk Δ = sw_2step − dsa_2step = 408.7 − 365.5 = **43.2 ms/GiB** → 推断比 = 43.2/516 = **8.4%**；disk2mem Δ = 423.6 − 387.4 = **36.2** → ≈**7.0%**（对称假设分母，待 resume 实测修正）。交叉验证：参照臂省比例 1 − 38.7/85.0 = 54.5% × 真实搬运段 71.5 = 38.9/516 = 7.5%，同量级，推断稳健。**净收益判据**：两步重构（给 DSA 腾用户态跳）新增 staging 跳成本 74~78 > DSA 省回 36~43 ms/GiB → dsa_2step 仍高于 sw_1step：mem2disk 净亏 **+10.4%** / disk2mem **+10.7%**（60MB 级窗口为负收益；60MB≤64 冷页口径下 DML 固定开销摊薄少、收益偏保守，放大窗口或改善摊薄，但不改变结构判定）。每行附 `/proc/self/io` 实测落盘字节（rw_*_per_gb=1.0 证明全真读盘）。

## 附录：代码位置索引（replay_offload.c 行号为当前版本近似值）

| 机制 | 函数/位置 | 行号 |
|------|----------|------|
| fc 双口径枚举 | `list_fc_pids` | ≈L462 |
| nonce 块扫描（15B 重叠） | `nonce_scan_pid` | ≈L495 |
| 同文件链式段组收集 | `collect_ram_segs` | ≈L556 |
| 进程+段组定位 | `find_by_nonce` | ≈L633 |
| 线性偏移→hva（foff 拼接查表） | `lin_to_hva` | ≈L892 |
| .b0loc 编码解析/校验/全扫 | `b0loc_parse16`/`b0loc_check`/`b0loc_scan_full` | ≈L966/988/1011 |
| 只读 GET（memset 契约） | `get_dirty_raw`/`bits_of` | ≈L1115/1125 |
| 候选位图→线性偏移 | `collect_slot_pages_ex` | ≈L1138 |
| **布局求解器** | `probe_dirty_bitmap_nonce` | ≈L1173 |
| dump 尾模型行 / GO 行 | `do_probe` | ≈L1660/1667 |
| 臂侧模型应用（不变式） | `do_arm` + `g_dump_order` | ≈L1706 / ≈L180 |
| 搬运后 checksum | `verify_dst` | ≈L1518 |
| 哨兵三件套写入 | replay_run.sh | ≈L344-376 |
| probe 调用/DIRTYSRC 解析 | replay_run.sh | ≈L383-404 |
| 三臂编排（rc 语义） | replay_run.sh `run_arm` | ≈L184 |
