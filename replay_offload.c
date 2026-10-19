// replay_offload.c — 阶段 4.5 B 线：运行态回放 harness（透明插桩 · 只读 · 零源码改动）
//
// 原理：运行态（aenv pause 之前）guest 已制脏完、随即 idle，firecracker 的 guest
//       RAM 就是 rw-p 的 COW 私有页（实测 mmap /dev/ublkbN MAP_PRIVATE），本进程在
//       AgentENV 之外「重放」同一段搬运，对比三条路径搬同一批页的 CPU 成本（B2 三臂）：
//   ① pvm  = process_vm_readv 循环（复刻 AgentENV 现状基线：orchestrator 用它从
//            firecracker 读 dirty 匿名内存）。连续线性偏移且 firecracker 侧 hva
//            连续时合并为一次 readv，上限 --pvm-chunk-kb（真实 iovec 合并粒度待
//            A1 bpftrace 实测校准，默认 64KB 为假设值，CSV 留痕）。不依赖 fd，
//            memfd/anon 两种 RAM 模式均原生可用。
//   ② mmap = memfd 模式：pidfd_getfd 偷出 guest RAM 的 memfd → 本地 PROT_READ
//            直映 + CPU memcpy（隔离「直映本身」的收益）。
//            anon 模式：匿名 mmap 无 fd、物理上无法映射进外部进程 → 一次性预取
//            页集合到本地缓冲（计时窗口外），rep 内 memcpy —— 与真实集成
//            「orchestrator 同进程直映、每快照只付搬运成本」等价（真实场景直接
//            持有指针，预取成本为零），note 列留痕 lbuf=1。
//   ③ dsa  = ② 的源 + DML batch（N≥48，复用 dsa_snapshot_offload.c 的描述符模式）。
//
// 纪律（不可违反）：
//   - 全程只读：pidfd_getfd / mmap PROT_READ / process_vm_readv / pread，不写
//     firecracker 的任何状态；KVM 侧只调 KVM_GET_DIRTY_LOG，绝不调
//     KVM_CLEAR_DIRTY_LOG（会破坏 AgentENV 的增量快照链）。
//   - 运行态调用：aenv pause 会把 guest RAM COW 页 flush+unmap（读源消失），
//     三臂回放必须在 pause 之前读运行态 COW 页。
//
// 两种模式：
//   probe 模式（--probe）：B0 RAM 发现（三路，见下）+ B1 只读 dirty log。
//     每轮制脏后跑一次（重复 GET_DIRTY_LOG 结果不一致），把页集合 dump 到
//     --dump-pages 文件，供同轮三个 arm 进程读同一批页。
//     B0 三路（--nonce-hex 提供时走路 0；否则按 1→2 顺序降级）：
//       路 0 nonce（共享服务器推荐，实测定位的双重失效根因见 find_by_nonce 注释）：
//            replay_run.sh 让 guest 每轮制脏前写 /dev/shm/.b0nonce（16B 随机数），
//            probe 侧枚举全机 firecracker 进程、在可读大段内内容搜索（16B 随机
//            碰撞概率为零）——命中进程 = 目标 fc（替代失效的 uuid 匹配：patch 版
//            命令行不含沙箱 uuid，7 个 fc 并存时会错抓别人），命中段所在「同文件
//            链式连续段组」= guest RAM（替代失效的匿名大小匹配：真实后端是 mmap
//            /dev/ublkbN，有路径且拆两段 VMA）。段组 hva 连续 → base+off 线性
//            模型，复用 anon 模式全套（含下方魔数校验双保险）。
//       路 1 memfd（上游 firecracker）：pidfd_getfd 偷 memfd → 直映 + 首页验证。
//       路 2 anonmap（无 nonce 时降级）：/proc/maps 按 slot 大小匹配匿名可读映射推断
//            hva 基址（唯一候选才接受，≥16MiB 的其他匿名映射打参考诊断）+ 魔数
//            内容校验：replay_run.sh 让 guest 每轮制脏前写 /dev/shm/.b0magic
//            （16B 魔数 ×4096 = 64KB tmpfs），probe 在页集合里逐页扫首 16B，命中
//            即内容级证明基址推断正确——大小匹配可能被 guard page 偏移欺骗，魔数
//            不会；nonce/anon 两模式共用该校验（miss → NOGO reason=magic-miss，
//            数据完整性优先：基址错则三臂读的全是错页，数字毫无意义）。arm 模式
//            不重复校验（魔数页可能在采样中丢失），依赖同轮内 maps 稳定 +
//            probe 已锚定。
//     stdout 协议行（供脚本解析）：
//       memfd：B0=GO ram_src=memfd ram_mib=<n> segs=<n> kvmvm=<0|1> name=<memfd名>
//       nonce：B0=GO ram_src=nonce ram_mib=<n> base0=0x.. base1=0x.. file=<路径|anon> kvmvm=<0|1> magic=hit@0x..
//       anon ：B0=GO ram_src=anonmap ram_mib=<n> base0=0x.. base1=0x.. kvmvm=<0|1> magic=hit@0x..
//       DIRTYSRC=bitmap|whole pages=<n> mb=<x>
//     失败：B0=NOGO reason=...（nonce-miss / nonce-no-fc / nonce-seg /
//            nonce-ram-too-big / steal-or-map / magic-miss ...，exit 1）。
//     dirty log 拿不到真实脏页（AgentENV capture 已消费位图 / ring 模式 EINVAL）时
//     自动退化为整段 RAM 采样（口径偏保守，README 4.5 B1 已论证三种机制均安全）。
//   arm 模式（--arm pvm|mmap|dsa --pages FILE）：跑单臂 --repeat 次，每次搬完做
//     64 点抽样 checksum（参考读与被测路径机制独立：memfd 模式 = pread memfd；
//     anon 模式 = pread /proc/<fc_pid>/mem，被测路径是 process_vm_readv/memcpy），
//     stdout 逐次输出 CSV 行（无表头）：
//       arm,src,pages,mb,chunk_kb,flush,irep,cpu_ms,wall_ms,cpu_ms_per_gb,cpu_util,cksum,note
//
// 采样口径：bitmap 全量时不采样（真实脏页集合）；超过 --subset-mb 或 whole 退化时
//   用「段式采样」——随机段起点 × 连续 SEG_PAGES(1MB) 页。理由：guest 内 dd 顺序写
//   /dev/shm，dirty 页在 memfd 上大段连续（slot0: memfd offset = gpa），纯随机页采样
//   会把 pvm 臂的 64KB iovec 合并退化成逐页 4KB readv，基线失真。段式保住聚集形态；
//   段重叠时页重复搬运（总量恒定，三臂同集合，公平）。seed=round：跨臂同集合、
//   跨轮变化（xorshift64，可复现）。
//
// 口径：CPU = CLOCK_PROCESS_CPUTIME_ID（本进程全线程 user+sys，即 onCPU 时间）；
//       wall = CLOCK_MONOTONIC；冷页 = 每次 repeat 前 256MB 扫掠冲刷 LLC（--cold 1）。
//       计时窗口只包 run_arm_dispatch 一次调用：dst 预热/anon 预取/flush_llc/
//       checksum 校验全在窗口外，三臂同口径。cpu_util = cpu_ms/wall_ms（卸载度）。
//       dsa 臂默认 --dsa-wait idle(UMWAIT)：busy(BUSY_POLL) 自旋会污染 cpu_ms，
//       那种口径只用来看功耗/延迟，note 列会打 spin=1 标记。
// memslot 布局与线性偏移约定（firecracker x86_64 标准双 slot，README 4.5 B1 注记）：
//   页集合用「线性偏移」作页身份，双 RAM 模式统一：
//     slot0: gpa [0, 0xE0000000)     → 线性偏移 = gpa
//     slot1: gpa [4GB, ...)          → 线性偏移 = gpa - 4GB + 0xE0000000（顺序布局假设）
//   即 off ∈ [0xE0000000, ram) ⟺ slot1；线性空间上限 = guest RAM 字节。
//   memfd 模式：线性偏移 = memfd 偏移（firecracker 顺序布局）；
//   anon 模式：slot0/slot1 各一条匿名映射，hva = base[slot] + (off − slot 线性基)。
//   guest 内存 ≤3584MiB 时只有 slot0，无布局假设；slot1 换算越界自动 WARN 跳过。
//
// 退出码：0 正常；1 设置/参数/环境错误；2 checksum 不一致（编排脚本应立即中止）；
//         3 DML/DSA 臂失败（未编译 -DUSE_DML 或 DSA 不可用，编排脚本降级跳过 dsa 臂）。
//
// 用法示例（命令写单行，避免行尾反斜杠触发 -Wcomment 多行注释警告）：
//   # probe（运行态；共享服务器推荐 --nonce-hex 自动定位进程+RAM）：
//   sudo ./replay_offload --nonce-hex <32hex> --probe --dump-pages pages_r1.txt --subset-mb 256 --seed 1
//   # 三臂（读同一份页集合，--nonce-hex 同 probe；--guest-mib 仅供交叉校验）：
//   sudo ./replay_offload --nonce-hex <32hex> --arm pvm  --pages pages_r1.txt --repeat 5 --src-tag whole
//   sudo ./replay_offload --nonce-hex <32hex> --arm mmap --pages pages_r1.txt --repeat 5 --src-tag whole
//   sudo ./replay_offload --nonce-hex <32hex> --arm dsa  --pages pages_r1.txt --repeat 5 --src-tag whole
//   # 无 nonce 降级（上游 memfd 环境 / 单 fc 机器；匿名 mmap 环境须 --fc-pid + --guest-mib）：
//   sudo ./replay_offload --fc-pid <fc_pid> --probe --guest-mib 3584 --dump-pages pages_r1.txt --subset-mb 256 --seed 1
//
// 编译（replay_run.sh 自动完成，此处留档；与脚本一致含 -lstdc++——DML 依赖）：
//   带 DML：gcc -O2 -Wall -DUSE_DML -I ~/.local/include replay_offload.c -o replay_offload -L ~/.local/lib -ldml -ldmlhl -luuid -pthread -lstdc++
//   无 DML：gcc -O2 -Wall replay_offload.c -o replay_offload -pthread（dsa 臂 exit 3）

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

#ifdef USE_DML
#include <dml/dml.h>
#endif

#define PAGE_SIZE     4096ULL
#define FLUSH_BYTES   (256ULL << 20)   /* LLC 冲刷缓冲（> L3，冷页口径） */
#define X86_HOLE_END  0xE0000000ULL    /* 3.5GB：x86_64 MMIO hole 起点 */
#define MAX_SLOTS     4
#define MAX_MAP_SEGS  64
#define CKSUM_SAMPLES 64               /* checksum 均匀抽样点数 */
#define MAX_REPEAT    64
#define MAX_PAGES     (16ULL << 20)    /* 回放页数上限（64GB guest RAM），防御 */
#define SEG_PAGES     256              /* 段式采样段长：256 页 = 1MB */
#define ALLOFFS_CAP0  (1U << 16)
#define B0_MAGIC      "DSAB0BASECALIBv1!"  /* 16B 恰好：anon 基址魔数校验（见文件头 B0 双路） */
#define B0_MAGIC_LEN  16
/* .b0loc 位置编码页（16 页）：页首 16B = "B0LOC" + 8 位页号(00..15) + "END"。
   guest 每轮制脏前写 /dev/shm/.b0loc——probe 用它做 gpa→hva 映射的端到端
   内容校验（布局求解门禁，见 probe_dirty_bitmap_nonce）；魔数只证明「读到
   目标 fc 的 RAM」，位置编码才能证明「dirty 位图的 gpa 语义与我们的线性
   偏移模型一致」（详见 qa.md）。 */
#define B0LOC_PAGES   16
#define B0LOC_MIN_HIT 12                  /* 门限：16 页中 ≥12 命中判通过 */

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434            /* 内核 ≥5.6 */
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

/* ---------------- 全局状态 ---------------- */

static pid_t    g_fc_pid;
static int      g_pidfd = -1;          /* firecracker 的 pidfd */
static int      g_memfd = -1;          /* 偷出的 guest RAM memfd（只读使用；仅 memfd 模式） */
static int      g_vmfd  = -1;          /* 偷出的 kvm-vm fd（仅 probe 用，只 GET） */
static char     g_memfd_name[512];
static uint64_t g_memfd_size;

/* guest RAM 双路（AgentENV patch 版 firecracker = 匿名 mmap + ublk，无 memfd，见文件头） */
enum { RAM_MEMFD = 0, RAM_ANON = 1 };
static int      g_ram_mode = RAM_MEMFD;   /* RAM_MEMFD：偷 fd 直映 / RAM_ANON：映射推断 */
static long     g_ram_mib;                /* --guest-mib：anon 推断与 slot 页数依据 */
static uint64_t g_ram_limit;              /* 线性偏移空间上限字节（= guest RAM 字节） */
static uint64_t g_lin_pages;              /* g_ram_limit / PAGE（页集合偏移上限） */
static uint64_t g_anon_base[2];           /* anon：slot0/slot1 匿名映射的 firecracker hva 基址 */
static uint8_t *g_lbuf;                   /* anon：mmap/dsa 臂本地预取源缓冲 */
static int      g_proc_mem = -1;          /* anon：/proc/<fc_pid>/mem 只读（魔数扫描/verify 参考） */
static char     g_ram_file[160];          /* nonce 模式：RAM 段组文件名（/dev/ublkbN；空 = 匿名） */
static char     g_nogo_reason[32] = "steal-or-map";  /* B0=NOGO reason=（发现失败细化） */

/* nonce 段组模式（g_nrsegs > 0）：RAM 段组明细 + 布局求解结果。
   g_rsegs 的排列即「线性偏移→hva」映射模型：文件后端组按 foff 升序（布局
   约定无关：slot 顺序布局与 gpa 直存两种文件排布下，foff 升序拼接都等于
   线性偏移空间）；匿名组无 foff，地址正/逆双候选由 .b0loc 校验裁定。
   g_slot0_bytes = slot0 字节数（位图位→线性偏移换算用；probe 求解后固化，
   dump 文件尾 #B0ORDER 行传给臂进程）。 */
enum { B0_ORDER_FOFF = 0, B0_ORDER_ASC = 1, B0_ORDER_DESC = 2 };
static struct ram_seg *g_rsegs;          /* find_by_nonce 填充（malloc），排列 = 映射模型 */
static int      g_nrsegs;
static int      g_b0_order = B0_ORDER_FOFF;  /* 段组排列（FOFF/ASC/DESC），恒 = g_rsegs 当前排列 */
static uint64_t g_slot0_bytes;           /* 布局解：slot0 字节数；0 = 未解出 */
static int      g_loc_hits = -1;         /* .b0loc 校验命中数（-1 = 未校验/不适用） */
static int      g_dump_order = -1;       /* load_pages 解析的 dump 尾 #B0ORDER（-1 = 无该行）；
                                            臂进程 do_arm 用 set_seg_order 应用——不能在
                                            load_pages 里直接改 g_b0_order：那会破坏
                                            「g_b0_order == g_rsegs 实际排列」不变式，
                                            使 set_seg_order 幂等检查误跳过重排 */

/* --nonce-hex：每轮制脏时 guest 内写的 16B 随机数（内容定位锚）。
   共享服务器上多 firecracker 并存且命令行无沙箱 uuid（find_fc 的 uuid 匹配失效），
   用内容级搜索定位目标进程与 guest RAM：16B 随机碰撞概率为零（见 find_by_nonce）。 */
static uint8_t g_nonce[16];
static int      g_nonce_len;              /* 0 = 未提供（走原双路） */
static uint8_t *g_map = MAP_FAILED;    /* memfd 的本地 PROT_READ 直映（仅 memfd 模式） */
static uint8_t *g_bm;                  /* dirty log 位图缓冲（按线性总页数分配，安全上界） */
static uint8_t *g_dst;                 /* 回放目标缓冲 */
static uint64_t *g_offs;               /* 回放页的线性偏移（页身份，双模式统一），升序 */
static uint64_t g_noffs;
static char     g_note[128];           /* CSV note 列（自动去逗号） */

struct map_seg { uint64_t vstart, vend, foff; };
static struct map_seg g_segs[MAX_MAP_SEGS];
static int      g_nsegs;

/* ---------------- 基础工具 ---------------- */

static double cpu_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double wall_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 256MB 双向扫掠冲刷 LLC（冷页口径，与 dsa_snapshot_offload.c 一致） */
static void flush_llc(void)
{
    static uint8_t *dummy;
    if (!dummy) dummy = malloc(FLUSH_BYTES);
    if (!dummy) return;
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < FLUSH_BYTES; i += 64) dummy[i] = (uint8_t)i;
    for (size_t i = 0; i < FLUSH_BYTES; i += 64) sink += dummy[i];
    (void)sink;
}

static uint64_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t xr(uint64_t *s)        /* xorshift64，非零种子不出零 */
{
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return *s = x;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int pidfd_open_(pid_t pid)      { return (int)syscall(__NR_pidfd_open, pid, 0); }
static int pidfd_getfd_(int pfd, int f){ return (int)syscall(__NR_pidfd_getfd, pfd, f, 0); }

static void usage(const char *p)
{
    fprintf(stderr,
        "用法（详见文件头注释）：\n"
        "  %s [--fc-pid <pid>] [--nonce-hex <32hex>] --probe --dump-pages <file>\n"
        "       [--guest-mib 3584] [--subset-mb 256] [--seed 1]\n"
        "       （共享服务器多 fc 并存：推荐 --nonce-hex（自动定位进程+RAM，见文件头路 0）；\n"
        "        无 nonce 时匿名 mmap 环境（AgentENV）须 --fc-pid 且必须 --guest-mib）\n"
        "  %s [--fc-pid <pid>] [--nonce-hex <32hex>] --arm pvm|mmap|dsa --pages <file>\n"
        "       [--repeat 5] [--cold 1] [--pvm-chunk-kb 64] [--batch-n 32]\n"
        "       [--dsa-wait busy|idle] [--src-tag tag] [--guest-mib 3584]\n"
        "       （--dsa-wait 默认 idle=UMWAIT：cpu_ms 为 CLOCK_PROCESS_CPUTIME_ID 的\n"
        "         onCPU 数；busy=BUSY_POLL 自旋会使 cpu_ms 虚高，仅用于功耗/延迟口径）\n",
        p, p);
}

/* ---------------- B0：fd 发现与直映 ---------------- */

/* 扫 /proc/<fc_pid>/fd，readlink 目标以 prefix 开头的 fd 全部 pidfd_getfd 偷出。
   names[i] 存 readlink 原文。返回个数（0=没有，-1=目录打不开）。 */
static int steal_fds_by_prefix(const char *prefix, int *fds, char names[][512], int max)
{
    char dir[64], path[320], link[512];
    snprintf(dir, sizeof dir, "/proc/%d/fd", (int)g_fc_pid);
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "[b0] opendir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/proc/%d/fd/%s", (int)g_fc_pid, e->d_name);
        ssize_t r = readlink(path, link, sizeof link - 1);
        if (r <= 0) continue;
        link[r] = 0;
        if (strncmp(link, prefix, strlen(prefix)) != 0) continue;
        int local = pidfd_getfd_(g_pidfd, atoi(e->d_name));
        if (local < 0) {
            fprintf(stderr, "[b0] pidfd_getfd(%s): %s（需 root/ptrace 权限）\n", link, strerror(errno));
            continue;
        }
        if (n < max) { fds[n] = local; snprintf(names[n], sizeof names[n], "%s", link); n++; }
        else close(local);
    }
    closedir(d);
    return n;
}

/* 解析 firecracker 的 /proc/<pid>/maps，记录目标 memfd 的映射区间
   （memfd offset → firecracker 虚拟地址，pvm 臂需要）。 */
static int parse_maps(void)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)g_fc_pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[b0] open %s: %s\n", path, strerror(errno));
        return -1;
    }
    g_nsegs = 0;
    uint64_t cov = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e, off, ino;
        char perms[8], dev[64];
        int rest = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %63s %lu %n",
                   &s, &e, perms, &off, dev, &ino, &rest) < 6 || rest <= 0)
            continue;
        char *p = line + rest;
        while (*p == ' ') p++;
        p[strcspn(p, "\n")] = 0;
        if (strncmp(p, "/memfd:", 7) != 0 || !strstr(p, g_memfd_name)) continue;
        if (g_nsegs < MAX_MAP_SEGS) {
            g_segs[g_nsegs].vstart = s;
            g_segs[g_nsegs].vend   = e;
            g_segs[g_nsegs].foff   = off;
            cov += (e - s);
            g_nsegs++;
        } else {
            fprintf(stderr, "[b0] WARN: memfd 映射段超过 %d，截断\n", MAX_MAP_SEGS);
        }
    }
    fclose(f);
    if (g_nsegs == 0) {
        fprintf(stderr, "[b0] firecracker maps 中无 %s 映射（pvm 臂不可用）\n", g_memfd_name);
        return -1;
    }
    fprintf(stderr, "[b0] maps 命中 %d 段，覆盖 %.1f%%（含权限分段计数）\n",
            g_nsegs, 100.0 * (double)cov / (double)g_memfd_size);
    return 0;
}

/* ---------------- B0 路 2：匿名映射推断（AgentENV patch 版） ---------------- */

/* 各 memslot 字节数（从 g_ram_limit = guest RAM 字节推导）：
   nonce 段组模式：布局由 probe 的 .b0loc 校验实测求解（g_slot0_bytes），
   不再假设 hole 边界——patch 版 fc 的 memslot 划分未实证，固定假设会整体
   错位（见 probe_dirty_bitmap_nonce）。
   其他模式（memfd / anonmap 降级）：上游布局 slot0 = min(ram, 3.5G)；
   slot1 = ram > 3.5G ? ram - 3.5G : 0（不存在） */
static uint64_t slot_bytes_of(int slot)
{
    if (g_ram_mode == RAM_ANON && g_nrsegs > 0) {
        if (g_slot0_bytes == 0) return 0;           /* 未求解（仅 probe 求解前瞬态） */
        return slot == 0 ? g_slot0_bytes : g_ram_limit - g_slot0_bytes;
    }
    if (g_ram_limit <= X86_HOLE_END) return slot == 0 ? g_ram_limit : 0;
    return slot == 0 ? X86_HOLE_END : g_ram_limit - X86_HOLE_END;
}

static uint64_t slot_pages_of(int slot) { return slot_bytes_of(slot) / PAGE_SIZE; }

/* /proc/<fc_pid>/mem 只读打开（魔数扫描 + anon 模式 verify 参考，与被测路径
   process_vm_readv 机制独立）。 */
static int open_proc_mem(void)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/mem", (int)g_fc_pid);
    g_proc_mem = open(path, O_RDONLY);
    if (g_proc_mem < 0) {
        fprintf(stderr, "[b0] open %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* 在 /proc/<pid>/maps 找大小落在 [want, want+2MiB) 的匿名（无路径）可读映射。
   匿名 mmap 无 fd 可偷，只能按大小匹配推断 guest RAM slot 的 hva 基址；
   窗口容忍尾部 guard page / madvise 边界；≥16MiB 的其他匿名映射打参考诊断
   （帮助人工判断歧义来源）。唯一候选才接受（0 个或多个 = 歧义拒绝）。
   返回 0 成功；-1 失败并写 why。 */
static int find_anon_base(uint64_t want, uint64_t *out, char *why, size_t whylen)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)g_fc_pid);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(why, whylen, "open-maps:%s", strerror(errno)); return -1; }
    uint64_t cand = 0;
    int ncand = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e, off, ino;
        char perms[8], dev[64];
        int rest = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %63s %lu %n",
                   &s, &e, perms, &off, dev, &ino, &rest) < 6)
            continue;
        if (perms[0] != 'r') continue;               /* 只要求可读：运行态 COW 页 rw-p，可读 */
        char *p = line + rest;
        while (*p == ' ') p++;
        if (*p != '\n' && *p != 0) continue;                 /* 有路径 = 非匿名 */
        uint64_t sz = e - s;
        if (sz >= want && sz < want + (2ULL << 20)) {
            cand = s;
            ncand++;
            fprintf(stderr, "[b0] 匿名映射候选 0x%lx-0x%lx（%luMiB，want=%lluMiB）\n",
                    s, e, sz >> 20, (unsigned long long)(want >> 20));
        } else if (sz >= (16ULL << 20)) {
            fprintf(stderr, "[b0] 跳过大匿名映射 0x%lx-0x%lx（%luMiB）\n",
                    s, e, sz >> 20);
        }
    }
    fclose(f);
    if (ncand == 1) { *out = cand; return 0; }
    snprintf(why, whylen, "slot-size-match(cand=%d,want=%lluMiB)",
             ncand, (unsigned long long)(want >> 20));
    return -1;
}

/* ---------------- B0 路 0：nonce 内容定位（共享服务器多 fc 并存） ---------------- */

/* 背景（实测定位的 2025-10 服务器 B 线 SMOKE 双重失效根因）：
   ① 进程发现失效：AgentENV patch 版 firecracker 命令行不含沙箱 uuid（只有
      --api-sock 实例随机名 agentenv-fc-XXXXXX），共享服务器 7 个 fc 并存 →
      replay_run.sh find_fc 的 uuid 匹配失效，降级「全机唯一」时错抓别人的
      沙箱（偷错 kvm-vm fd，B1 GET_DIRTY_LOG 状态自然不对）；
   ② RAM 后端误判：真实 guest RAM 是 mmap /dev/ublkbN（有路径的映射）且拆成
      两段地址连续 VMA（实测 512MiB@foff 0xc0000000 + 3GiB@foff 0），上面
      find_anon_base 的「有路径 = 非匿名」过滤直接排除真 RAM。
   nonce 路（--nonce-hex）：制脏时 guest 内写 16B 随机数（/dev/shm/.b0nonce），
   probe 侧枚举全机 firecracker、在其可读大段内内容搜索——16B 随机数内容级
   碰撞概率为零，命中进程 = 目标 fc（替代 uuid 匹配），命中段所在「同文件
   链式连续段组」= guest RAM（替代匿名大小匹配）。
   段组 hva 地址连续 → base + off 线性模型成立，复用 anon 模式全套
   （lbuf 预取 / verify / 魔数内容校验双保险）。 */

struct ram_seg { uint64_t s, e, foff; char file[160]; };  /* foff：文件偏移（匿名 = 0） */

/* /proc/<pid>/cmdline 任一 argv 含 "firecracker"？
   AgentENV patch 版 fc 的 comm 不保证是 "firecracker"（实测共享机上 pgrep -f
   可见的 fc 多于 comm 匹配到的——首轮 SMOKE 即因此漏扫目标 fc），cmdline 兜底。
   误抓无害：nonce 内容搜索是最终裁决，不含 nonce 的进程扫一遍即跳过。 */
static int pid_has_fc_cmdline(pid_t pid)
{
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = 0;                                       /* cmdline = \0 分隔的 argv 串 */
    for (char *p = buf; p < buf + r; p += strlen(p) + 1)
        if (strstr(p, "firecracker")) return 1;
    return 0;
}

/* 枚举全机 firecracker 进程：comm == "firecracker" 或 cmdline 含 "firecracker"
   （双口径取并集；readdir 每进程至多遍历一次，天然去重），最多 cap 个。 */
static int list_fc_pids(pid_t *out, int cap)
{
    int n = 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid == getpid()) continue;                /* 自身排除（cmdline 误含字样） */
        int is_fc = 0;
        char path[300];                              /* 容纳 /proc/ + NAME_MAX(255) + /comm */
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            char name[64] = "";
            if (fgets(name, sizeof name, f)) {
                name[strcspn(name, "\n")] = 0;
                if (!strcmp(name, "firecracker")) is_fc = 1;
            }
            fclose(f);
        }
        if (!is_fc && pid_has_fc_cmdline(pid)) is_fc = 1;
        if (is_fc) out[n++] = pid;
    }
    closedir(d);
    return n;
}

/* 在 <pid> 的可读大段（≥16MiB：guest RAM 量级，过滤栈/堆等小段）内搜 g_nonce。
   只要求可读不要求 rw：运行态 guest RAM = rw-p COW 私有页，本工具对 fc 内存
   只读不写（pread / pvm 读），rw 从来不是必需。2025-10 实测：pause 会把该
   COW 段 flush+unmap（fc 全 maps 最大段只剩 3MiB r-xp）→ 可读≥16MiB 段 0 个、
   nonce 全 miss —— 这正是三臂回放必须改到运行态（pause 之前）的根因。
   1MiB 块 pread(/proc/<pid>/mem) + memmem；块间回退 15B 重叠（防 16B nonce
   恰跨块边界漏检）。命中返回所在段起始 hva；miss 返回 0；进程不可读返回
   (uint64_t)-1（并发启停的别家 fc，跳过）。 */
static uint64_t nonce_scan_pid(pid_t pid)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return (uint64_t)-1;
    snprintf(path, sizeof path, "/proc/%d/mem", (int)pid);
    int mem = open(path, O_RDONLY);
    if (mem < 0) {
        fprintf(stderr, "[b0] nonce-scan pid=%d：/proc/pid/mem 打不开（%s）\n",
                (int)pid, strerror(errno));
        fclose(f);
        return (uint64_t)-1;
    }

    static uint8_t buf[1 << 20];
    uint64_t hit = 0;
    uint32_t nseg = 0, nfail = 0;                     /* 诊断：候选段数 / pread 失败数 */
    uint64_t rbytes = 0;
    int ferrno = 0;                                   /* 首个 pread 失败的 errno */
    uint64_t maxsz = 0;                               /* 诊断：全 maps 最大段（任意权限） */
    char maxperms[8] = "";
    while (!hit && fgets(line, sizeof line, f)) {
        unsigned long s, e, off, ino;
        char perms[8], dev[64];
        if (sscanf(line, "%lx-%lx %7s %lx %63s %lu", &s, &e, perms, &off, dev, &ino) < 6)
            continue;
        if ((uint64_t)(e - s) > maxsz) {              /* 先记全 maps 最大段（miss 诊断用） */
            maxsz = e - s;
            snprintf(maxperms, sizeof maxperms, "%s", perms);
        }
        if (perms[0] != 'r') continue;                /* 只要求可读：运行态 COW 页 rw-p */
        if (e - s < (16ULL << 20)) continue;
        nseg++;
        for (uint64_t o = 0; o < e - s && !hit; ) {
            uint64_t left = e - s - o;
            size_t want = left > (1ULL << 20) ? (1ULL << 20) : (size_t)left;
            ssize_t r = pread(mem, buf, want, (off_t)(s + o));
            if (r <= 0) {                             /* 该段不可读（已被 munmap 等）→ 下一段 */
                if (!ferrno) ferrno = errno;
                nfail++;
                break;
            }
            rbytes += (uint64_t)r;
            if (memmem(buf, (size_t)r, g_nonce, (size_t)g_nonce_len)) { hit = s; break; }
            o += (uint64_t)r;
            if (o < e - s && o >= 15) o -= 15;        /* 块间重叠 15B */
        }
    }
    close(mem);
    fclose(f);
    if (!hit) {                                       /* miss 必打明细，根因一眼可辨 */
        fprintf(stderr, "[b0] nonce-scan pid=%d：可读≥16MiB 段 %u 个、实读 %lluMiB、"
                        "pread 失败 %u 次", (int)pid, nseg,
                (unsigned long long)(rbytes >> 20), nfail);
        if (ferrno) fprintf(stderr, "（首错 %s）", strerror(ferrno));
        if (!nseg && maxsz)                           /* 无候选段：最大段线索辨因
                                                        （rw-p 段消失 = pause 已 unmap，
                                                        须在 pause 前运行态扫） */
            fprintf(stderr, "；全 maps 最大段 %lluMiB(%s)",
                    (unsigned long long)((maxsz + (1ULL << 20) - 1) >> 20), maxperms);
        fprintf(stderr, "、未命中\n");
    }
    return hit;
}

/* 收集 <pid> maps 中与命中段（起始 hva = hit_s）同文件（路径字符串相同，空 =
   匿名组）的可读段（≥1MiB；只要求可读——运行态 guest RAM 是 rw-p COW 页，
   本工具只读不写），再从命中段向两边做「地址链式连续」扩展
   （前段尾 == 后段头）。/proc/maps 天然按地址升序，无需排序；真实 guest RAM
   常被按权限/映射时机拆成多条地址连续 VMA（实测 ublk 后端拆两段），链式扩展
   还原完整段组。返回段组段数（0 = 命中段已不在 maps）；base/total 输出段组
   首地址与总字节，out 段组明细（含文件名）。 */
static int collect_ram_segs(pid_t pid, uint64_t hit_s,
                            struct ram_seg *out, int cap,
                            uint64_t *base, uint64_t *total)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    static struct ram_seg all[MAX_MAP_SEGS * 2];
    int nall = 0, hit_idx = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e, off, ino;
        char perms[8], dev[64];
        int rest = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %63s %lu %n",
                   &s, &e, perms, &off, dev, &ino, &rest) < 6)
            continue;
        if (perms[0] != 'r') continue;               /* 只要求可读：运行态 COW 页 rw-p */
        if (e - s < (1ULL << 20)) continue;           /* 微小段（vdso/vvar 等）排除 */
        char *p = line + rest;
        while (*p == ' ') p++;
        p[strcspn(p, "\n")] = 0;
        if ((uint64_t)s == hit_s) hit_idx = nall;
        if (nall < MAX_MAP_SEGS * 2) {
            all[nall].s = s;
            all[nall].e = e;
            all[nall].foff = off;                    /* 文件偏移：布局求解/foff 序映射用 */
            snprintf(all[nall].file, sizeof all[nall].file, "%s", p);
            nall++;
        }
    }
    fclose(f);
    if (hit_idx < 0) return 0;

    int idxs[MAX_MAP_SEGS * 2], cnt = 0;              /* 同文件候选（地址升序保持） */
    for (int i = 0; i < nall; i++)
        if (!strcmp(all[i].file, all[hit_idx].file)) idxs[cnt++] = i;
    int pos = -1;
    for (int i = 0; i < cnt; i++) if (idxs[i] == hit_idx) { pos = i; break; }
    if (pos < 0) return 0;

    int lo = pos, hi = pos;                           /* 链式连续扩展 */
    while (lo > 0 && all[idxs[lo - 1]].e == all[idxs[lo]].s) lo--;
    while (hi + 1 < cnt && all[idxs[hi]].e == all[idxs[hi + 1]].s) hi++;
    if (hi - lo + 1 > cap) {
        fprintf(stderr, "[b0] nonce WARN：段组 %d 段超上限 %d，截断\n", hi - lo + 1, cap);
        hi = lo + cap - 1;
    }
    uint64_t t = 0;
    for (int i = lo; i <= hi; i++) {
        out[i - lo] = all[idxs[i]];
        t += all[idxs[i]].e - all[idxs[i]].s;
    }
    *base  = all[idxs[lo]].s;
    *total = t;
    return hi - lo + 1;
}

/* 段组按 foff 升序比较（文件后端组的映射模型序） */
static int cmp_seg_foff(const void *a, const void *b)
{
    const struct ram_seg *x = a, *y = b;
    return x->foff < y->foff ? -1 : x->foff > y->foff ? 1 : 0;
}

/* nonce 主流程：定位目标 fc 进程 + guest RAM 段组，就绪 anon 模式全局态。
   --fc-pid 指定的进程优先扫（只影响扫描顺序，不影响结果）；
   命中 pid 与 --fc-pid 不一致时以 nonce 为准（WARN 留痕，修正 g_fc_pid）。
   RAM 段组实测大小即 g_ram_limit（--guest-mib 仅交叉校验）。
   段组 → g_rsegs 即「线性偏移→hva」映射模型：文件后端按 foff 升序拼接
   （布局约定无关：无论文件是 slot 顺序布局还是 gpa 直存，foff 升序拼接
   都等于「slot 顺序、hole 挤掉」的线性偏移空间）；匿名组无 foff，保持
   地址序（正/逆双候选由 probe 的 .b0loc 校验裁定）。
   g_slot0_bytes 先给 hole 假设默认值，probe 位图求解会实测覆盖
   （见 probe_dirty_bitmap_nonce）。
   成功返回 0；失败返回 -1（g_nogo_reason 已细化）。 */
static int find_by_nonce(void)
{
    pid_t pids[64];
    int np = list_fc_pids(pids, 64);
    if (np == 0) {
        fprintf(stderr, "[b0] nonce：全机无 firecracker 进程\n");
        snprintf(g_nogo_reason, sizeof g_nogo_reason, "nonce-no-fc");
        return -1;
    }
    if (g_fc_pid > 0) {
        for (int i = 1; i < np; i++)
            if (pids[i] == g_fc_pid) {
                pid_t t = pids[0]; pids[0] = pids[i]; pids[i] = t;
                break;
            }
    }
    pid_t hit_pid = 0;
    uint64_t hit_s = 0;
    int nunread = 0;                                  /* /proc/pid/mem 打不开的进程数 */
    for (int i = 0; i < np; i++) {
        uint64_t h = nonce_scan_pid(pids[i]);
        if (h == (uint64_t)-1) { nunread++; continue; } /* 别家 fc 启停竞态/不可读 → 跳过 */
        if (h != 0) { hit_pid = pids[i]; hit_s = h; break; }
    }
    if (!hit_pid) {
        if (nunread == np)
            fprintf(stderr, "[b0] nonce：%d 个 firecracker 的 /proc/pid/mem 全部打不开"
                            "（权限/yama ptrace_scope/dumpable？用 root 跑）\n", np);
        else
            fprintf(stderr, "[b0] nonce：扫 %d 个 firecracker 均未命中（另 %d 个不可读；"
                            "各 pid 明细见上方 nonce-scan 行）——nonce 没写进 guest？"
                            "pause 后 fc 的 COW 页已 unmap？\n", np, nunread);
        snprintf(g_nogo_reason, sizeof g_nogo_reason, "nonce-miss");
        return -1;
    }
    if (g_fc_pid > 0 && hit_pid != g_fc_pid)
        fprintf(stderr, "[b0] nonce WARN：--fc-pid=%d 的进程不含 nonce，实际命中 pid=%d"
                        "（以 nonce 为准——共享服务器上 uuid 匹配失效，见文件头路 0）\n",
                (int)g_fc_pid, (int)hit_pid);
    else if (g_fc_pid <= 0)
        fprintf(stderr, "[b0] nonce：未指定 --fc-pid，由 nonce 定位 pid=%d\n", (int)hit_pid);
    g_fc_pid = hit_pid;

    static struct ram_seg segs[MAX_MAP_SEGS];
    uint64_t base = 0, total = 0;
    int nseg = collect_ram_segs(g_fc_pid, hit_s, segs, MAX_MAP_SEGS, &base, &total);
    if (nseg <= 0) {
        fprintf(stderr, "[b0] nonce：命中段所在段组收集失败（maps 已变？）\n");
        snprintf(g_nogo_reason, sizeof g_nogo_reason, "nonce-seg");
        return -1;
    }
    if (total / PAGE_SIZE > MAX_PAGES) {
        fprintf(stderr, "[b0] nonce：RAM 段组 %lluMiB 超回放上限（%llu 页 > %llu）\n",
                (unsigned long long)(total >> 20),
                (unsigned long long)(total / PAGE_SIZE), (unsigned long long)MAX_PAGES);
        snprintf(g_nogo_reason, sizeof g_nogo_reason, "nonce-ram-too-big");
        return -1;
    }
    /* 段组 → 映射模型（见函数头注释）。g_anon_base 仅作兼容显示（nonce 模式
       lin_to_hva 走 g_rsegs 查表，不用它）。 */
    g_rsegs = malloc((size_t)nseg * sizeof(struct ram_seg));
    if (!g_rsegs) {
        fprintf(stderr, "[b0] nonce：段组缓冲分配失败（%d 段）\n", nseg);
        snprintf(g_nogo_reason, sizeof g_nogo_reason, "nonce-alloc");
        return -1;
    }
    memcpy(g_rsegs, segs, (size_t)nseg * sizeof(struct ram_seg));
    g_nrsegs = nseg;
    g_slot0_bytes = total > X86_HOLE_END ? X86_HOLE_END : total;  /* 默认值，probe 实测覆盖 */
    if (segs[0].file[0]) {
        qsort(g_rsegs, (size_t)nseg, sizeof(struct ram_seg), cmp_seg_foff);
        g_b0_order = B0_ORDER_FOFF;
    } else {
        g_b0_order = B0_ORDER_ASC;                    /* 匿名组：地址序（DESC 由校验裁定） */
    }
    g_anon_base[0] = base;
    g_anon_base[1] = 0;
    g_ram_limit = total;
    g_lin_pages = total / PAGE_SIZE;
    g_ram_mode = RAM_ANON;                            /* 复用 anon 全套（含魔数校验双保险） */
    snprintf(g_ram_file, sizeof g_ram_file, "%s", segs[0].file);
    if (g_ram_mib > 0 && ((uint64_t)g_ram_mib << 20) != total)
        fprintf(stderr, "[b0] nonce WARN：--guest-mib=%ld 与 nonce 实测 %lluMiB 不一致"
                        "（以 nonce 实测为准）\n",
                g_ram_mib, (unsigned long long)(total >> 20));
    fprintf(stderr, "[b0] nonce 定位 OK：fc_pid=%d ram=%lluMiB 段组基址=0x%llx "
                    "file=%s（%d 段连续）\n",
            (int)g_fc_pid, (unsigned long long)(total >> 20), (unsigned long long)base,
            g_ram_file[0] ? g_ram_file : "(匿名)", nseg);
    for (int i = 0; i < nseg; i++)                    /* 段明细：布局谜题时人工对账用 */
        fprintf(stderr, "[b0]   段%d hva=[0x%llx,0x%llx) size=%lluMiB foff=0x%llx%s\n",
                i, (unsigned long long)g_rsegs[i].s, (unsigned long long)g_rsegs[i].e,
                (unsigned long long)((g_rsegs[i].e - g_rsegs[i].s) >> 20),
                (unsigned long long)g_rsegs[i].foff,
                g_rsegs[i].file[0] ? "" : "（匿名）");
    return 0;
}

/* ---------------- B0：RAM 发现（三路） ---------------- */

/* B0 RAM 发现（三路）+ 可选 kvm-vm fd。
   路 0 nonce（--nonce-hex，共享服务器推荐）：guest 内 16B 随机数内容定位——
     自动锁定目标 fc 进程（替代失效的 uuid 匹配）与 guest RAM 段组（替代失效
     的匿名大小匹配——实测 patch 版后端是 mmap /dev/ublkbN，有路径且拆多段
     VMA），详见 find_by_nonce 注释。
   路 1 memfd（上游 firecracker）：偷最大 memfd → mmap 只读直映 + 首页验证。
   路 2 anonmap（无 nonce 时降级）：/proc/maps 按 slot 大小匹配匿名可写映射
     推断 hva 基址（唯一候选才接受）；基址正确性由 do_probe 的魔数内容校验
     锚定（见文件头），此处只做推断。
   路 0 成功即跳过路 1/2（RAM 已定位），复用 anon 模式全套；三路都就绪
   g_ram_limit/g_lin_pages（页集合线性偏移上限）。
   kvm-vm fd 偷取三路共用（B1 dirty log 与 RAM 后端无关，patch 版同样有）。 */
static int discover_and_setup(int want_vmfd)
{
    /* ---- 路 0：nonce 内容定位（最先跑：可能修正 g_fc_pid，后续 pidfd_open/
       偷 fd 都要用修正后的 pid；给了 nonce 但定位失败 → 立即中止）---- */
    int nonce_ok = 0;
    if (g_nonce_len == 16) {
        if (find_by_nonce() != 0) return -1;
        nonce_ok = 1;
    }

    g_pidfd = pidfd_open_(g_fc_pid);
    if (g_pidfd < 0) {
        fprintf(stderr, "[b0] pidfd_open(%d): %s（需 root）\n", (int)g_fc_pid, strerror(errno));
        return -1;
    }

    /* ---- 路 1：memfd（上游 firecracker；nonce 已定位 RAM 则跳过偷取）---- */
    int memfd_ok = 0;
    int fds[16];
    char names[16][512];
    int n = nonce_ok ? 0 : steal_fds_by_prefix("/memfd:", fds, names, 16);
    if (n > 0) {
        int best = -1;
        uint64_t best_sz = 0;
        for (int i = 0; i < n; i++) {
            struct stat st;
            if (fstat(fds[i], &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size > best_sz) {
                best_sz = (uint64_t)st.st_size;
                best = i;
            }
        }
        if (best >= 0 && best_sz >= (1ULL << 20)) {
            g_memfd = fds[best];
            g_memfd_size = best_sz;
            snprintf(g_memfd_name, sizeof g_memfd_name, "%s", names[best]);
            char *sp = strstr(g_memfd_name, " (deleted)");
            if (sp) *sp = 0;
            if (strncmp(g_memfd_name, "/memfd:", 7) == 0)
                memmove(g_memfd_name, g_memfd_name + 7, strlen(g_memfd_name) - 6);
            fprintf(stderr, "[b0] memfd=%s size=%lluMiB（其余 %d 个 memfd 关闭）\n",
                    g_memfd_name, (unsigned long long)(best_sz >> 20), n - 1);
            for (int i = 0; i < n; i++) if (i != best) close(fds[i]);

            g_map = mmap(NULL, g_memfd_size, PROT_READ, MAP_SHARED, g_memfd, 0);
            if (g_map == MAP_FAILED) {
                fprintf(stderr, "[b0] mmap memfd: %s\n", strerror(errno));
                return -1;
            }
            uint8_t first[64];
            if (pread(g_memfd, first, sizeof first, 0) != (ssize_t)sizeof first) {
                fprintf(stderr, "[b0] pread 首页验证失败: %s\n", strerror(errno));
                return -1;
            }
            volatile uint64_t v = *(volatile uint64_t *)g_map;  /* 触碰首字节，验证直映可读 */
            (void)v;

            g_ram_mode  = RAM_MEMFD;
            g_ram_limit = g_memfd_size;
            g_lin_pages = g_memfd_size / PAGE_SIZE;
            if (g_lin_pages > MAX_PAGES) {
                fprintf(stderr, "[b0] memfd %lluMiB 超回放上限（%llu 页 > %llu）\n",
                        (unsigned long long)(g_memfd_size >> 20),
                        (unsigned long long)g_lin_pages, (unsigned long long)MAX_PAGES);
                return -1;
            }
            memfd_ok = 1;
        } else {
            fprintf(stderr, "[b0] 有 memfd fd 但无 >=1MB 者 → 尝试匿名映射推断\n");
            for (int i = 0; i < n; i++) close(fds[i]);
        }
    } else if (n == 0) {
        fprintf(stderr, nonce_ok
                        ? "[b0] nonce 已定位 RAM（anon 模式），跳过 memfd 偷取\n"
                        : "[b0] firecracker 进程无 /memfd: fd"
                          "（AgentENV patch 版：匿名 mmap + ublk）→ 匿名映射推断\n");
    } else {
        fprintf(stderr, "[b0] 扫 /proc/fd 失败 → 尝试匿名映射推断\n");
    }

    /* ---- 路 2：匿名映射大小匹配推断（仅无 nonce 且路 1 未成功时）---- */
    if (!memfd_ok && !nonce_ok) {
        if (g_ram_mib <= 0) {
            fprintf(stderr, "[b0] 匿名推断需要 --guest-mib（guest 配置内存 MiB）\n");
            return -1;
        }
        g_ram_mode  = RAM_ANON;
        g_ram_limit = (uint64_t)g_ram_mib << 20;
        g_lin_pages = g_ram_limit / PAGE_SIZE;
        if (g_lin_pages > MAX_PAGES) {
            fprintf(stderr, "[b0] guest RAM %ldMiB 超回放上限\n", g_ram_mib);
            return -1;
        }
        char why[128] = "";
        if (find_anon_base(slot_bytes_of(0), &g_anon_base[0], why, sizeof why) != 0) {
            fprintf(stderr, "[b0] slot0（%lluMiB）匿名映射推断失败：%s\n",
                    (unsigned long long)(slot_bytes_of(0) >> 20), why);
            return -1;
        }
        if (slot_bytes_of(1) > 0 &&
            find_anon_base(slot_bytes_of(1), &g_anon_base[1], why, sizeof why) != 0) {
            fprintf(stderr, "[b0] slot1（%lluMiB）匿名映射推断失败：%s\n",
                    (unsigned long long)(slot_bytes_of(1) >> 20), why);
            return -1;
        }
        fprintf(stderr, "[b0] anon 推断 OK：ram=%ldMiB base0=0x%llx%s"
                        "（基址正确性由 probe 魔数校验锚定）\n",
                g_ram_mib, (unsigned long long)g_anon_base[0],
                slot_bytes_of(1) > 0 ? "" : "（单 slot）");
    }
    /* anon 模式（路 2 推断 / 路 0 nonce）共用 /proc/<fc_pid>/mem：
       魔数扫描 + verify 参考读。nonce 路 g_fc_pid 已修正，此处打开即正确。 */
    if (g_ram_mode == RAM_ANON && g_proc_mem < 0 && open_proc_mem() != 0) return -1;

    /* ---- kvm-vm fd（三路共用，B1 只读 GET）---- */
    if (want_vmfd) {
        int vfd[4];
        char vnames[4][512];
        int vn = steal_fds_by_prefix("anon_inode:kvm-vm", vfd, vnames, 4);
        if (vn <= 0) {
            fprintf(stderr, "[b1] 未找到 kvm-vm fd → 整段 RAM 采样（保守口径）\n");
        } else {
            g_vmfd = vfd[0];  /* firecracker 一进程一 VM，取第一个 */
            for (int i = 1; i < vn; i++) close(vfd[i]);
        }
    }
    return g_ram_mode == RAM_MEMFD ? parse_maps() : 0;
}

/* memfd 偏移 → firecracker 虚拟地址（memfd 模式 pvm 臂用）。找不到返回 0。 */
static uint64_t remote_addr(uint64_t off)
{
    for (int i = 0; i < g_nsegs; i++)
        if (off >= g_segs[i].foff &&
            off < g_segs[i].foff + (g_segs[i].vend - g_segs[i].vstart))
            return g_segs[i].vstart + (off - g_segs[i].foff);
    return 0;
}

/* 线性偏移 → firecracker hva（双模式统一，pvm 臂 / 预取 / 魔数扫描 / verify 用）。
   memfd 模式：查 g_segs（memfd 偏移→fc 虚拟地址），找不到返回 0。
   nonce 段组模式（g_nrsegs > 0）：g_rsegs 按序拼接查表——段 i 覆盖线性偏移
   [C_i, C_i+size)，hva = seg.s + (off - C_i)。foff 升序拼接对「文件 slot 顺序
   布局」与「文件 gpa 直存」都正确（g_rsegs 排列即模型，probe 求解/校验后
   固化，dump 尾 #B0ORDER 传给臂进程），不再假设 hva 与线性偏移同序——
   实测段组 foff 逆序排布（低地址段 foff 3G、高地址段 foff 0），旧的
   base+off 线性模型会整体错位 512MiB。找不到返回 0。
   anonmap 降级模式：slot 内 base + 相对偏移（基址已被 probe 魔数校验锚定）。 */
static uint64_t lin_to_hva(uint64_t off)
{
    if (g_ram_mode == RAM_MEMFD) return remote_addr(off);
    if (g_nrsegs > 0) {
        uint64_t c = 0;
        for (int i = 0; i < g_nrsegs; i++) {
            uint64_t sz = g_rsegs[i].e - g_rsegs[i].s;
            if (off >= c && off < c + sz) return g_rsegs[i].s + (off - c);
            c += sz;
        }
        return 0;
    }
    if (off < X86_HOLE_END) return g_anon_base[0] + off;
    return g_anon_base[1] + (off - X86_HOLE_END);
}

/* 魔数快扫（仅 anon 模式）：候选集合内逐页 pread 首 16B 找 B0_MAGIC。
   只在魔数恰在页首时命中——replay_run.sh 用 yes|head 写（17B 周期），不保证
   页首对齐 → 只作快路径，miss 必须再走 scan_magic_full 兜底。
   g_scan_ok_pages 记成功读到 16B 的页数（miss 诊断：0 = /proc/<pid>/mem 读不了）。 */
static uint64_t g_scan_ok_pages;
static uint64_t g_scan_full_bytes;

static uint64_t scan_magic(const uint64_t *offs, uint64_t n)
{
    g_scan_ok_pages = 0;
    if (g_proc_mem < 0) return ~0ULL;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t buf[B0_MAGIC_LEN];
        uint64_t hva = lin_to_hva(offs[i]);
        if ((uint64_t)pread(g_proc_mem, buf, B0_MAGIC_LEN, (off_t)hva) != B0_MAGIC_LEN)
            continue;
        g_scan_ok_pages++;
        if (memcmp(buf, B0_MAGIC, B0_MAGIC_LEN) == 0) return offs[i];
    }
    return ~0ULL;
}

/* 魔数全线性空间流式扫描（兜底，memmem 任意位置匹配，不依赖页首对齐）：
   1MiB 块 pread + memmem，3584MiB ≈ 0.5s（probe 计时窗口外）。
   whole 模式下替代逐页快扫（91 万次 pread → 3584 次）。魔数跨块边界的单次
   出现会漏，但文件内 ~3800 次重复出现，总有一次完整落在块内。
   g_scan_full_bytes 记成功读到的字节数（miss 诊断）。
   命中返回魔数所在页的线性偏移；未命中返回 ~0ULL。 */
static uint64_t scan_magic_full(void)
{
    g_scan_full_bytes = 0;
    if (g_proc_mem < 0) return ~0ULL;
    const uint64_t total = g_lin_pages * PAGE_SIZE;
    const uint64_t bufsz = 1ULL << 20;
    uint8_t *buf = malloc(bufsz);
    if (!buf) return ~0ULL;
    for (uint64_t off = 0; off < total; off += bufsz) {
        uint64_t end = off + bufsz;
        if (end > total) end = total;
        if (off < X86_HOLE_END && end > X86_HOLE_END) end = X86_HOLE_END;  /* hva 不跨 hole */
        ssize_t r = pread(g_proc_mem, buf, end - off, (off_t)lin_to_hva(off));
        if (r < (ssize_t)B0_MAGIC_LEN) continue;
        g_scan_full_bytes += (uint64_t)r;
        uint8_t *p = memmem(buf, (size_t)r, B0_MAGIC, B0_MAGIC_LEN);
        if (p) {
            uint64_t hit = off + (uint64_t)(p - buf);
            free(buf);
            return hit & ~(uint64_t)(PAGE_SIZE - 1);   /* 归到页粒度线性偏移 */
        }
    }
    free(buf);
    return ~0ULL;
}

/* ---------------- .b0loc 位置编码校验（gpa→hva 端到端门禁） ---------------- */

/* buf 16B 是否合法 .b0loc 页首（"B0LOC" + 8 位数字页号 + "END"）？
   是则 *pageno 出页号（0..15）。 */
static int b0loc_parse16(const uint8_t *buf, int *pageno)
{
    if (memcmp(buf, "B0LOC", 5) != 0 || memcmp(buf + 13, "END", 3) != 0)
        return 0;
    int v = 0;
    for (int k = 5; k < 13; k++) {
        if (buf[k] < '0' || buf[k] > '9') return 0;
        v = v * 10 + (buf[k] - '0');
    }
    if (v < 0 || v >= B0LOC_PAGES) return 0;
    *pageno = v;
    return 1;
}

/* .b0loc 校验（核心）：offs（全量 dirty 集合，subset 抽样前）逐页 pread 16B，
   经当前映射模型 lin_to_hva 读页首，统计 distinct 页号命中数。
   校验力：dirty 位图位 = KVM 语义的 gpa → 我们换算成线性偏移 → 模型映射到
   hva → 读到的恰是 .b0loc 位置编码 → 「位图语义 + 换算 + 映射」全链路正确。
   错位模型下命中 ≈ 0：需 ≥12 个 dirty 页经错误映射恰好落在 16 个 .b0loc 页首
   （单页概率 ~dirty/ram ≈ 个位数 %，12 页同时命中概率天文小）。
   提前退出：命中达门限即返回（正确候选毫秒级；错误候选扫全量，见候选数）。
   返回命中数；-1 = /proc/<pid>/mem 不可读。 */
static int b0loc_check(const uint64_t *offs, uint64_t n)
{
    if (g_proc_mem < 0) return -1;
    unsigned seen = 0;                                 /* bit i = 页号 i 已命中 */
    int hits = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t buf[16];
        uint64_t hva = lin_to_hva(offs[i]);
        if (hva == 0) continue;
        if ((uint64_t)pread(g_proc_mem, buf, 16, (off_t)hva) != 16) continue;
        int pg;
        if (b0loc_parse16(buf, &pg) && !(seen & (1u << pg))) {
            seen |= 1u << pg;
            if (++hits >= B0LOC_MIN_HIT) return hits;  /* 提前退出 */
        }
    }
    return hits;
}

/* .b0loc 全空间流式扫（loc-miss 诊断用：区分「没写进 guest / 被回收」vs
   「布局谜题」）。流式 memmem "B0LOC" + 逐命中格式校验，覆盖整个线性空间。
   注意：全空间扫本身没有映射校验力（内容总能被扫到），只回答「.b0loc 在不
   在这个进程的 RAM 里」。返回 distinct 页号命中数。 */
static int b0loc_scan_full(void)
{
    if (g_proc_mem < 0) return -1;
    const uint64_t total = g_lin_pages * PAGE_SIZE;
    const uint64_t bufsz = 1ULL << 20;
    uint8_t *buf = malloc(bufsz);
    if (!buf) return -1;
    unsigned seen = 0;
    for (uint64_t off = 0; off < total; off += bufsz) {
        uint64_t end = off + bufsz;
        if (end > total) end = total;
        ssize_t r = pread(g_proc_mem, buf, end - off, (off_t)lin_to_hva(off));
        if (r < 16) continue;
        /* 页首对齐检查：.b0loc 页首恰在 4K 边界（guest 整页写入） */
        for (ssize_t o = (ssize_t)((PAGE_SIZE - (off & (PAGE_SIZE - 1))) & (PAGE_SIZE - 1));
             o + 16 <= r; o += (ssize_t)PAGE_SIZE) {
            int pg;
            if (b0loc_parse16(buf + o, &pg)) seen |= 1u << pg;
        }
    }
    free(buf);
    return __builtin_popcount(seen);
}

/* ---------------- B1：只读 dirty log ---------------- */

/* 只读 KVM_GET_DIRTY_LOG：位图写入 g_bm，返回置位总数；-1 = ioctl 失败
   （slot 不存在 / ring 模式返回 EINVAL）。注意每次 GET 覆盖 g_bm，
   调用方须 GET 后立即消费再 GET 下一个 slot。位图只写该 slot 页数对应的
   字数——计数也按该 slot 字数，否则会把上一 slot 残留位误计入。
   严禁 KVM_CLEAR_DIRTY_LOG——会破坏 AgentENV 增量快照链。 */
static long get_dirty_slot(int slot)
{
    struct kvm_dirty_log kdl;
    memset(&kdl, 0, sizeof kdl);
    kdl.slot = (uint32_t)slot;
    kdl.dirty_bitmap = g_bm;
    if (ioctl(g_vmfd, KVM_GET_DIRTY_LOG, &kdl) != 0)
        return -1;
    long bits = 0;
    uint64_t words = (slot_pages_of(slot) + 63) / 64;
    for (uint64_t w = 0; w < words; w++)
        bits += __builtin_popcountll(((const uint64_t *)g_bm)[w]);
    return bits;
}

/* 当前 g_bm（slot 的位图）→ 线性偏移，追加到 out。
   slot0: 线性偏移 = gpa
   slot1: 线性偏移 = gpa - 4GB + 3.5GB（顺序布局假设；越界 WARN 跳过） */
static uint64_t collect_slot_pages(int slot, uint64_t *out, uint64_t cap)
{
    uint64_t n = 0, base = (slot == 0) ? 0 : X86_HOLE_END;
    uint64_t pages = slot_pages_of(slot);
    uint64_t words = (pages + 63) / 64;
    for (uint64_t w = 0; w < words && n < cap; w++) {
        uint64_t x = ((const uint64_t *)g_bm)[w];
        while (x && n < cap) {
            unsigned b = __builtin_ctzll(x);
            x &= x - 1;
            uint64_t off = base + (((w << 6) + b) << 12);
            if (off >= g_ram_limit) {
                if (slot != 0)
                    fprintf(stderr, "[b1] WARN: slot%d 位图越出线性空间（slot1 顺序布局假设"
                            "可能不成立，精确口径建议 guest ≤3584MiB 复测）\n", slot);
                continue;
            }
            out[n++] = off;
        }
    }
    return n;
}

/* ---------------- nonce 段组模式：位图快照 + 布局求解 ---------------- */

/* 段按 hva 起址升/降序比较（匿名组排列候选） */
static int cmp_seg_hva(const void *a, const void *b)
{
    const struct ram_seg *x = a, *y = b;
    return x->s < y->s ? -1 : x->s > y->s ? 1 : 0;
}
static int cmp_seg_hva_desc(const void *a, const void *b) { return -cmp_seg_hva(a, b); }

static const char *order_name(int o)
{
    return o == B0_ORDER_FOFF ? "foff" : o == B0_ORDER_ASC ? "asc" : "desc";
}

/* 重排 g_rsegs 为指定排列 = 切换「线性偏移→hva」映射模型。
   FOFF = foff 升序（文件后端组）；ASC/DESC = hva 地址正/逆序（匿名组候选，
   foff 全 0 无从排序）。幂等。 */
static void set_seg_order(int order)
{
    if (order == g_b0_order) return;
    if (order == B0_ORDER_FOFF) qsort(g_rsegs, (size_t)g_nrsegs, sizeof(struct ram_seg), cmp_seg_foff);
    else if (order == B0_ORDER_ASC) qsort(g_rsegs, (size_t)g_nrsegs, sizeof(struct ram_seg), cmp_seg_hva);
    else qsort(g_rsegs, (size_t)g_nrsegs, sizeof(struct ram_seg), cmp_seg_hva_desc);
    g_b0_order = order;
}

/* 原生 GET（只 ioctl 不计数——nonce 模式 slot 页数待求解，位计数移到候选
   确定后按截断字数做）。返回 0 = 位图在 g_bm；-1 = 失败（EINVAL = slot
   不存在 / ring 模式）。调用方须先 memset(g_bm)：KVM 只写该 slot 真实页数
   对应的字数，不清零缓冲——不 memset 会把上一 slot 残留位当本 slot 的
   （幻影页）。严禁 CLEAR（保护 AgentENV 增量快照链）。 */
static int get_dirty_raw(int slot)
{
    struct kvm_dirty_log kdl;
    memset(&kdl, 0, sizeof kdl);
    kdl.slot = (uint32_t)slot;
    kdl.dirty_bitmap = g_bm;
    return ioctl(g_vmfd, KVM_GET_DIRTY_LOG, &kdl) == 0 ? 0 : -1;
}

/* 位图快照前 pages 页的置位总数（候选确定后按该候选 slot 页数截断计数） */
static long bits_of(const void *bm, uint64_t pages)
{
    uint64_t words = (pages + 63) / 64;
    long bits = 0;
    for (uint64_t w = 0; w < words; w++)
        bits += __builtin_popcountll(((const uint64_t *)bm)[w]);
    return bits;
}

/* 位图快照 bm（该 slot 的）→ 线性偏移，按候选 slot0 字节数 s0 追加到 out。
   slot0：位 i → 线性偏移 i*4K（gpa = 线性偏移）；slot1：位 j → s0 + j*4K
   （hole 挤掉的顺序拼接语义——线性偏移空间的定义）。位图有效字数按候选
   页数截断（截断外残留必为 0：GET 前 memset 保证）。返回收集页数。 */
static uint64_t collect_slot_pages_ex(const void *bm, int slot, uint64_t s0,
                                      uint64_t *out, uint64_t cap)
{
    uint64_t base = (slot == 0) ? 0 : s0;
    uint64_t pages = ((slot == 0) ? s0 : g_ram_limit - s0) / PAGE_SIZE;
    uint64_t words = (pages + 63) / 64;
    uint64_t n = 0;
    for (uint64_t w = 0; w < words && n < cap; w++) {
        uint64_t x = ((const uint64_t *)bm)[w];
        while (x && n < cap) {
            unsigned b = __builtin_ctzll(x);
            x &= x - 1;
            out[n++] = base + (((w << 6) + b) << 12);
        }
    }
    return n;
}

/* nonce 段组模式的 dirty 位图收集 + memslot 布局求解（B1 核心）。
   为什么不能沿用固定 slot 布局：patch 版 fc 的 memslot 划分与「文件偏移→
   VMA」映射顺序都未实证——实测段组 foff 逆序排布（低地址段 foff 3G、
   高地址段 foff 0）就与「hva 与线性偏移同序」矛盾，任何固定假设都可能
   整体错位 512MiB（tmpfs 制脏页还偏好落 4G 以上 slot → 位图有、映射丢）。
   改为实测求解：
     ① GET slot0/slot1 位图快照（GET 前 memset 防残留幻影；slot 不存在 =
        EINVAL；slot2/3 防御探测）；严禁 CLEAR
     ② 候选 slot0 字节数 s0 ∈ {总 RAM（单 slot）, X86_HOLE_END（hole 边界）,
        段组 foff 序累计边界}，按 slot1 存在性过滤（s0 < 总 ⇒ slot1 必须在；
        s0 == 总 ⇒ slot1 必须 EINVAL）
     ③ 逐候选重建线性偏移集合 → .b0loc 内容校验（gpa→hva 端到端门禁；
        错位候选 ≈ 0 命中，见 b0loc_check）；匿名组额外枚举地址正/逆双排列
     ④ 胜者固化（g_slot0_bytes + g_rsegs 排列）；全灭 → NOGO loc-miss
        （宁失败不出错数据，诊断见输出）
   返回：>0 求解成功（*out 已填，页数即返回值）；0 = 位图全空（调用方退化
   whole 口径）；-1 = NOGO（g_nogo_reason 已设）。 */
static int64_t probe_dirty_bitmap_nonce(uint64_t *out, uint64_t cap,
                                        char *note, size_t note_sz)
{
    const uint64_t bm_bytes = (g_lin_pages + 7) / 8;
    uint8_t *bm0 = malloc(bm_bytes), *bm1 = malloc(bm_bytes);
    uint64_t *tmp = malloc(cap * sizeof(uint64_t));
    if (!bm0 || !bm1 || !tmp) {
        free(bm0); free(bm1); free(tmp);
        snprintf(g_nogo_reason, sizeof g_nogo_reason, "alloc-bitmap");
        return -1;
    }
    memset(g_bm, 0, bm_bytes);
    int ok0 = get_dirty_raw(0) == 0;
    memcpy(bm0, g_bm, bm_bytes);
    memset(g_bm, 0, bm_bytes);
    int ok1 = get_dirty_raw(1) == 0;
    int e1 = errno;                                    /* slot1 GET 失败原因（EINVAL=不存在），须立即取 */
    memcpy(bm1, g_bm, bm_bytes);
    for (int s = 2; s < MAX_SLOTS; s++) {              /* 防御：非预期大 slot */
        memset(g_bm, 0, bm_bytes);
        if (get_dirty_raw(s) == 0)
            fprintf(stderr, "[b1] WARN：slot%d 也有 dirty 位图（非预期布局，"
                            "本工具只解 slot0/1）——口径可能不完整\n", s);
    }
    fprintf(stderr, "[b1] slot0 GET=%s slot1 GET=%s%s\n",
            ok0 ? "ok" : "FAIL", ok1 ? "ok" : "FAIL",
            ok1 ? "" : strerror(e1));
    free(g_bm); g_bm = NULL;                           /* 位图已快照，防误用 */
    if (!ok0) {                                        /* slot0 都拿不到 = 位图路线不可用 */
        free(bm0); free(bm1); free(tmp);
        return 0;
    }

    int any = 0;                                       /* 位图全空 → whole 退化 */
    for (uint64_t w = 0; w < bm_bytes / 8 && !any; w++)
        if (((uint64_t *)bm0)[w] || ((uint64_t *)bm1)[w]) any = 1;
    if (!any) {
        free(bm0); free(bm1); free(tmp);
        return 0;
    }

    /* 候选 s0（去重） */
    uint64_t cand[MAX_MAP_SEGS + 2];
    int ncand = 0;
    cand[ncand++] = g_ram_limit;                       /* 单 slot */
    if (X86_HOLE_END < g_ram_limit) cand[ncand++] = X86_HOLE_END;
    {
        uint64_t c = 0;
        for (int i = 0; i < g_nrsegs; i++) {           /* foff 序段边界（slot 顺序布局对齐段） */
            c += g_rsegs[i].e - g_rsegs[i].s;
            if (c == 0 || c >= g_ram_limit || c % PAGE_SIZE) continue;
            int dup = 0;
            for (int k = 0; k < ncand; k++) if (cand[k] == c) dup = 1;
            if (!dup) cand[ncand++] = c;
        }
    }
    int orders[2] = { B0_ORDER_ASC, B0_ORDER_DESC };
    int norders = (g_rsegs[0].file[0] == 0) ? 2 : 1;   /* 匿名组双排列；文件组 foff 序 */

    for (int oi = 0; oi < norders; oi++) {
        if (norders == 2) set_seg_order(orders[oi]);
        for (int ci = 0; ci < ncand; ci++) {
            uint64_t s0 = cand[ci];
            if (s0 == g_ram_limit && ok1) continue;    /* 单 slot 但 slot1 存在 → 矛盾 */
            if (s0 < g_ram_limit && !ok1) continue;    /* 双 slot 但 slot1 缺席 → 矛盾 */
            uint64_t n = collect_slot_pages_ex(bm0, 0, s0, tmp, cap);
            if (ok1) n += collect_slot_pages_ex(bm1, 1, s0, tmp + n, cap - n);
            if (n == 0) continue;
            int hits = b0loc_check(tmp, n);
            fprintf(stderr, "[b1]   布局候选 slot0=%lluMiB order=%s → loc=%d/%d"
                            "（%llu 页）\n",
                    (unsigned long long)(s0 >> 20), order_name(g_b0_order),
                    hits, B0LOC_PAGES, (unsigned long long)n);
            if (hits >= B0LOC_MIN_HIT) {               /* 胜出：固化模型 */
                g_slot0_bytes = s0;
                g_loc_hits = hits;
                memcpy(out, tmp, n * sizeof(uint64_t));
                long b0 = bits_of(bm0, s0 / PAGE_SIZE);
                long b1 = ok1 ? bits_of(bm1, (g_ram_limit - s0) / PAGE_SIZE) : 0;
                snprintf(note, note_sz, "s0:%ld s1:%ld ", b0, b1);
                fprintf(stderr, "[b1] 布局求解 OK：slot0=%lluMiB（%ld 脏页）"
                                "slot1=%s（%ld 脏页）loc=%d/%d → 共 %llu 脏页\n",
                        (unsigned long long)(s0 >> 20), b0,
                        ok1 ? "在" : "无", b1, hits, B0LOC_PAGES,
                        (unsigned long long)n);
                free(bm0); free(bm1); free(tmp);
                return (int64_t)n;
            }
        }
    }

    /* 全灭：先全空间扫 .b0loc 定位根因，再 NOGO */
    int full = b0loc_scan_full();
    fprintf(stderr, "[b1] .b0loc 校验全灭（所有候选 < %d/%d 命中）。\n",
            B0LOC_MIN_HIT, B0LOC_PAGES);
    if (full <= 0)
        fprintf(stderr, "     .b0loc 全空间扫描 %d 命中——没写进 guest RAM 或已被回收"
                        "（guest 无 swap 时 tmpfs 页驻留，最大可能是 aenv exec 失败被吞）"
                        "→ 查 replay_run.sh 的 .b0loc WARN 行；guest 工具链是否支持"
                        " printf/head。\n", full);
    else
        fprintf(stderr, "     .b0loc 在 RAM 中（全扫 %d/%d 页号）但所有布局候选映射"
                        "均错位 → 非预期 memslot/文件布局：把 /proc/%d/maps 与上方"
                        "候选诊断发给维护者（qa.md「失败诊断」节）。\n",
                full, B0LOC_PAGES, (int)g_fc_pid);
    snprintf(g_nogo_reason, sizeof g_nogo_reason, "loc-miss");
    free(bm0); free(bm1); free(tmp);
    return -1;
}

/* ---------------- 页集合文件 ---------------- */

static void load_pages(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(1); }
    uint64_t cap = ALLOFFS_CAP0;
    g_offs = malloc(cap * sizeof(uint64_t));
    if (!g_offs) { fprintf(stderr, "malloc offs\n"); exit(1); }
    char *line = NULL;
    size_t lc = 0;
    ssize_t r;
    uint64_t n = 0;
    while ((r = getline(&line, &lc, f)) != -1) {
        if (r <= 1) continue;                       /* 空行 */
        if (line[0] == '#') {                       /* 模型行：probe 传给臂的映射参数（应用见 do_arm） */
            if (!strncmp(line, "#B0ORDER ", 9)) {
                char od[8] = "";
                sscanf(line + 9, "%7s", od);
                if (!strcmp(od, "foff")) g_dump_order = B0_ORDER_FOFF;
                else if (!strcmp(od, "asc")) g_dump_order = B0_ORDER_ASC;
                else if (!strcmp(od, "desc")) g_dump_order = B0_ORDER_DESC;
                else fprintf(stderr, "pages 文件 #B0ORDER 未知值：%s\n", od);
            }
            continue;
        }
        if (n >= MAX_PAGES) { fprintf(stderr, "页数超上限 %llu\n", (unsigned long long)MAX_PAGES); exit(1); }
        char *end = NULL;
        uint64_t off = strtoull(line, &end, 16);
        if (end == line) continue;                  /* 非 hex 行容错跳过 */
        if (off % PAGE_SIZE) { fprintf(stderr, "偏移 0x%llx 非 4KB 对齐\n", (unsigned long long)off); exit(1); }
        if (off / PAGE_SIZE >= g_lin_pages) {
            fprintf(stderr, "偏移 0x%llx 超出 guest RAM 线性空间（%llu 页）\n",
                    (unsigned long long)off, (unsigned long long)g_lin_pages);
            exit(1);
        }
        if (n == cap) {
            cap <<= 1;
            g_offs = realloc(g_offs, cap * sizeof(uint64_t));
            if (!g_offs) { fprintf(stderr, "realloc offs\n"); exit(1); }
        }
        g_offs[n++] = off;
    }
    free(line);
    fclose(f);
    qsort(g_offs, n, sizeof(uint64_t), cmp_u64);
    g_noffs = n;
}

/* ---------------- B2：三臂 ---------------- */

/* anon 模式：mmap/dsa 臂无 fd 可映射（MAP_ANONYMOUS 无法进外部进程），把页集合
   一次性预取到本地缓冲 g_lbuf（合并连续且 hva 连续的偏移，process_vm_readv，
   1MiB/次上限；计时窗口外）。rep 内 memcpy/DML 读 g_lbuf —— 与真实集成中
   「orchestrator 同进程直映、每快照只付搬运成本」等价（真实场景直接持有
   指针，预取成本为零）。pvm 臂不走此路径（它本身就在测跨进程读成本）。 */
static int prefetch_lbuf(void)
{
    /* 必须 4KB 对齐：DML batch 子任务地址非 4KB 对齐时，DSA 硬件报
       DML_STATUS_PAGE_FAULT_ERROR=25（2026-10-16 实证：malloc 大块返回 mmap+16，
       batch 全灭；单任务不受影响）。posix_memalign 保证页对齐。 */
    if (posix_memalign((void **)&g_lbuf, 4096,
                       (size_t)g_noffs * (size_t)PAGE_SIZE) != 0) {
        fprintf(stderr, "[lbuf] posix_memalign 失败（%llu 页）\n", (unsigned long long)g_noffs);
        return -1;
    }
    uint64_t i = 0, n = g_noffs;
    while (i < n) {
        uint64_t start = i, bytes = PAGE_SIZE;
        /* 循环不变量：g_offs[i] == g_offs[start] + bytes - PAGE（且 hva 连续） */
        while (i + 1 < n && bytes + PAGE_SIZE <= (1ULL << 20) &&
               g_offs[i + 1] == g_offs[i] + PAGE_SIZE &&
               lin_to_hva(g_offs[i + 1]) == lin_to_hva(g_offs[i]) + PAGE_SIZE) {
            i++; bytes += PAGE_SIZE;
        }
        uint8_t *lptr = g_lbuf + start * PAGE_SIZE;
        uint64_t raddr = lin_to_hva(g_offs[start]);
        if (!raddr) {
            fprintf(stderr, "[lbuf] 线性偏移 0x%llx 在 firecracker 地址空间无映射\n",
                    (unsigned long long)g_offs[start]);
            return -1;
        }
        size_t done = 0;
        while (done < bytes) {
            struct iovec liov = { lptr + done, bytes - done };
            struct iovec riov = { (void *)(uintptr_t)(raddr + done), bytes - done };
            ssize_t r = process_vm_readv(g_fc_pid, &liov, 1, &riov, 1, 0);
            if (r <= 0) {
                fprintf(stderr, "[lbuf] process_vm_readv: %s（hva=0x%llx）\n",
                        strerror(errno), (unsigned long long)raddr);
                return -1;
            }
            done += (size_t)r;
        }
        i++;
    }
    fprintf(stderr, "[lbuf] 预取 %llu 页 → 本地缓冲（计时窗口外，note 留痕 lbuf=1）\n",
            (unsigned long long)g_noffs);
    return 0;
}

/* 第 i 个回放页的源指针：memfd 模式 = 本地直映；anon 模式 = 本地预取缓冲。 */
static const uint8_t *page_src(uint64_t i)
{
    return g_ram_mode == RAM_ANON ? g_lbuf + i * PAGE_SIZE
                                  : g_map + g_offs[i];
}

/* ① process_vm_readv 循环 = AgentENV 现状基线（orchestrator 同款 syscall）。
   连续线性偏移且 firecracker 侧 hva 连续（跨 slot 边界自动切分）时合并为一次
   readv，上限 chunk 字节；跨进程读支持部分读，循环补齐。 */
static int run_pvm(uint32_t chunk_kb)
{
    size_t chunk = (size_t)chunk_kb << 10;
    uint64_t i = 0, n = g_noffs, reads = 0;
    while (i < n) {
        uint64_t start = i, bytes = PAGE_SIZE;
        /* 循环不变量：g_offs[i] == g_offs[start] + bytes - PAGE（且 hva 连续） */
        while (i + 1 < n && bytes + PAGE_SIZE <= chunk &&
               g_offs[i + 1] == g_offs[i] + PAGE_SIZE &&
               lin_to_hva(g_offs[i + 1]) == lin_to_hva(g_offs[i]) + PAGE_SIZE) { i++; bytes += PAGE_SIZE; }
        uint8_t *lptr = g_dst + start * PAGE_SIZE;
        uint64_t raddr = lin_to_hva(g_offs[start]);
        if (!raddr) {
            fprintf(stderr, "[pvm] 线性偏移 0x%llx 在 firecracker 地址空间无映射\n",
                    (unsigned long long)g_offs[start]);
            return -1;
        }
        size_t done = 0;
        while (done < bytes) {
            struct iovec liov = { lptr + done, bytes - done };
            struct iovec riov = { (void *)(uintptr_t)(raddr + done), bytes - done };
            ssize_t r = process_vm_readv(g_fc_pid, &liov, 1, &riov, 1, 0);
            if (r <= 0) {
                fprintf(stderr, "[pvm] process_vm_readv: %s\n", strerror(errno));
                return -1;
            }
            done += (size_t)r;
        }
        reads++;
        i++;
    }
    snprintf(g_note, sizeof g_note, "reads=%llu", (unsigned long long)reads);
    return 0;
}

/* ② mmap 直映 + CPU 逐页 memcpy（隔离「直映本身」收益；anon 模式源 = 预取缓冲） */
static int run_mmap_cp(void)
{
    for (uint64_t i = 0; i < g_noffs; i++)
        memcpy(g_dst + i * PAGE_SIZE, page_src(i), PAGE_SIZE);
    snprintf(g_note, sizeof g_note, "%s",
             g_ram_mode == RAM_ANON ? "lbuf=1" : "");
    return 0;
}

/* ③ DML batch：直映源 + DSA 硬件搬运（复用 dsa_snapshot_offload.c 的描述符模式） */
#ifdef USE_DML
static dml_job_t *g_job;
static uint8_t   *g_batch;
static uint32_t   g_batch_n;

static int dsa_setup(uint32_t batch_n)
{
    uint32_t job_size = 0;
    dml_status_t st = dml_get_job_size(DML_PATH_HW, &job_size);
    if (st != DML_STATUS_OK || job_size == 0) {
        fprintf(stderr, "[dsa] dml_get_job_size: status=%u\n", (unsigned)st);
        return -1;
    }
    g_job = malloc(job_size);
    if (!g_job) return -1;
    memset(g_job, 0, job_size);    /* fault-in 全缓冲：CR 区域未映射时 DMA 写直接报错 */
    st = dml_init_job(DML_PATH_HW, g_job);
    if (st != DML_STATUS_OK) {
        fprintf(stderr, "[dsa] dml_init_job: status=%u（DSA 不可用 / libaccel-config 缺失？）\n", (unsigned)st);
        return -1;
    }
    g_job->operation = DML_OP_BATCH;    /* 必须先设 operation，dml_get_batch_size 依赖它 */
    uint32_t batch_bytes = 0;
    st = dml_get_batch_size(g_job, batch_n, &batch_bytes);
    if (st != DML_STATUS_OK || batch_bytes == 0) {
        fprintf(stderr, "[dsa] dml_get_batch_size(n=%u): status=%u\n", batch_n, (unsigned)st);
        return -1;
    }
    g_batch = malloc(batch_bytes);
    if (!g_batch) return -1;
    g_job->destination_first_ptr = g_batch;
    g_job->destination_length = batch_bytes;
    g_batch_n = batch_n;
    return 0;
}

static int run_dsa(int wait_idle)
{
    uint64_t n = g_noffs, bn = g_batch_n;
    uint64_t batches = n / bn, tail = n % bn;
    dml_status_t st;
    for (uint64_t b = 0; b < batches; b++) {
        for (uint64_t i = 0; i < bn; i++) {
            uint64_t k = b * bn + i;
            dml_batch_set_mem_move_by_index(g_job, (uint32_t)i,
                (uint8_t *)page_src(k), g_dst + k * PAGE_SIZE, PAGE_SIZE, 0);
        }
        st = dml_submit_job(g_job);    /* 异步提交，立即返回 */
        if (st != DML_STATUS_OK) { fprintf(stderr, "[dsa] submit: %u\n", (unsigned)st); return -1; }
        /* idle 语义映射 UMWAIT（低功耗等待）——DML 只有 BUSY_POLL/UMWAIT 两种枚举 */
        st = dml_wait_job(g_job, wait_idle ? DML_WAIT_MODE_UMWAIT : DML_WAIT_MODE_BUSY_POLL);
        if (st != DML_STATUS_OK) { fprintf(stderr, "[dsa] wait: %u\n", (unsigned)st); return -1; }
    }
    for (uint64_t k = batches * bn; k < n; k++)      /* 尾页 CPU 兜底 */
        memcpy(g_dst + k * PAGE_SIZE, page_src(k), PAGE_SIZE);
    snprintf(g_note, sizeof g_note, "tail=%llu%s%s", (unsigned long long)tail,
             g_ram_mode == RAM_ANON ? " lbuf=1" : "",
             wait_idle ? "" : " spin=1(busy-poll cpu_ms-inflated)");
    return 0;
}

static void dsa_teardown(void)
{
    if (g_job) { dml_finalize_job(g_job); free(g_job); g_job = NULL; }
    free(g_batch);
    g_batch = NULL;
}
#else
static int dsa_setup(uint32_t b)      { (void)b; fprintf(stderr, "[dsa] 未编译 -DUSE_DML，dsa 臂不可用\n"); return -1; }
static int run_dsa(int w)             { (void)w; return -1; }
static void dsa_teardown(void)        { }
#endif

static int run_arm_dispatch(const char *arm, uint32_t chunk_kb, int wait_idle)
{
    if (!strcmp(arm, "pvm"))  return run_pvm(chunk_kb);
    if (!strcmp(arm, "mmap")) return run_mmap_cp();
    return run_dsa(wait_idle);
}

/* 数据完整性仅为64点抽样：pvm/memfd用独立参考读；RAM_ANON的
   mmap/dsa对照各臂本地预取源，不检查当前guest，不能保证跨臂源内容一致
   或所有页正确。见CURRENT_STATUS.md。 */
static int verify_dst(const char *arm)
{
    uint64_t n = g_noffs;
    uint64_t samples = n < CKSUM_SAMPLES ? n : CKSUM_SAMPLES;
    uint8_t *ref = malloc(PAGE_SIZE);
    if (!ref) return 0;
    for (uint64_t i = 0; i < samples; i++) {
        uint64_t k = (n <= 1) ? 0 : (i * (n - 1)) / (samples - 1);
        const uint8_t *expected;
        if (g_ram_mode == RAM_ANON && strcmp(arm, "pvm") != 0) {
            /* mmap/dsa 臂：源是本地预取快照 g_lbuf。运行态下 guest 持续写，
               当前 guest RAM 会与快照漂移，pread 当前值产生 checksum 假阳性
               （2026-10-16 全量 r2 实证：mmap 臂过、dsa 臂挂）→ 对比源快照。
               源正确性已由 B0=GO 的 nonce/magic 定位保证。 */
            expected = page_src(k);
        } else {
            /* pvm 臂（源=当前 guest RAM）或 memfd（源=稳定快照）：pread 独立校验 */
            ssize_t got = g_ram_mode == RAM_ANON
                ? pread(g_proc_mem, ref, PAGE_SIZE, (off_t)lin_to_hva(g_offs[k]))
                : pread(g_memfd, ref, PAGE_SIZE, g_offs[k]);
            if (got != (ssize_t)PAGE_SIZE) { free(ref); return 0; }
            expected = ref;
        }
        if (fnv1a(g_dst + k * PAGE_SIZE, PAGE_SIZE) != fnv1a(expected, PAGE_SIZE)) {
            fprintf(stderr, "[verify] 采样点 k=%llu 不一致（arm=%s dst vs 源）\n",
                    (unsigned long long)k, arm);
            free(ref); return 0;
        }
    }
    free(ref);
    return 1;
}

/* ---------------- probe 模式 ---------------- */

static int do_probe(const char *dump_path, long subset_mb, long seed)
{
    if (discover_and_setup(1) != 0) { printf("B0=NOGO reason=%s\n", g_nogo_reason); return 1; }

    uint64_t *all = malloc((size_t)g_lin_pages * sizeof(uint64_t));
    if (!all) { printf("B0=NOGO reason=alloc-pages\n"); return 1; }
    uint64_t n_all = 0;
    const char *src = "whole";
    char slots_note[128] = "";

    if (g_vmfd >= 0) {
        g_bm = calloc(1, (size_t)((g_lin_pages + 7) / 8));
        if (!g_bm) { printf("B0=NOGO reason=alloc-bitmap\n"); return 1; }
        if (g_ram_mode == RAM_ANON && g_nrsegs > 0) {
            /* nonce 段组模式：位图快照 + memslot 布局求解（.b0loc 端到端门禁，
               见 probe_dirty_bitmap_nonce）。位图快照后 g_bm 已释放。 */
            int64_t r = probe_dirty_bitmap_nonce(all, g_lin_pages, slots_note, sizeof slots_note);
            if (r < 0) { printf("B0=NOGO reason=%s\n", g_nogo_reason); return 1; }
            n_all = (uint64_t)r;
            if (n_all > 0) src = "bitmap";
            else fprintf(stderr, "[b1] 位图为空或不可用（AgentENV capture 已消费 / ring 模式）"
                                 " → 退化整段 RAM 采样（保守口径；布局求解不适用，"
                                 "foff 序拼接模型天然不重不漏）\n");
        } else {
            /* memfd / anonmap 降级模式：上游标准布局，slot_bytes_of 直接可用 */
            int any_ok = 0;
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (slot_pages_of(s) == 0) continue;  /* 该 slot 不存在（单 slot 配置） */
                long bits = get_dirty_slot(s);       /* 只读 GET；GET 后立即消费 */
                if (bits < 0) {
                    fprintf(stderr, "[b1] slot%d GET_DIRTY_LOG: %s\n", s, strerror(errno));
                    continue;
                }
                any_ok = 1;
                if (bits > 0)
                    n_all += collect_slot_pages(s, all + n_all, g_lin_pages - n_all);
                char b2[32];
                snprintf(b2, sizeof b2, "s%d:%ld ", s, bits);
                if (strlen(slots_note) + strlen(b2) < sizeof slots_note) strcat(slots_note, b2);
            }
            fprintf(stderr, "[b1] slots(%s)→ %llu 脏页\n",
                    slots_note[0] ? slots_note : "none", (unsigned long long)n_all);
            if (any_ok && n_all > 0) src = "bitmap";
            else fprintf(stderr, "[b1] 位图为空或不可用（AgentENV capture 已消费 / ring 模式）"
                                 " → 退化整段 RAM 采样（保守口径）\n");
        }
    } else {
        fprintf(stderr, "[b1] 无 kvm-vm fd → 整段 RAM 采样\n");
    }

    if (n_all == 0) {                                /* whole：全部页 */
        if (g_lin_pages > MAX_PAGES) { printf("B0=NOGO reason=ram-too-big\n"); return 1; }
        for (uint64_t i = 0; i < g_lin_pages; i++) all[i] = i << 12;
        n_all = g_lin_pages;
    }

    /* anon 模式：魔数内容校验（在采样前做）。miss → NOGO：基址推断错则三臂读的
       全是错页，数字毫无意义（数据完整性优先）。
       bitmap 集合（n_all < 全部页）：先逐页快扫（毫秒级），miss 再全空间流式扫；
       whole 全页集合：逐页扫 = ram/4K 次 pread，直接走全空间流式扫。 */
    uint64_t magic_off = ~0ULL;
    if (g_ram_mode == RAM_ANON) {
        if (n_all < g_lin_pages) {
            magic_off = scan_magic(all, n_all);
            if (magic_off == ~0ULL)
                fprintf(stderr, "[b0] 集合快扫未命中（可读 %llu/%llu 页）→ 全空间流式扫兜底…\n",
                        (unsigned long long)g_scan_ok_pages, (unsigned long long)n_all);
        }
        if (magic_off == ~0ULL)
            magic_off = scan_magic_full();
        if (magic_off == ~0ULL) {
            printf("B0=NOGO reason=magic-miss\n");
            fprintf(stderr,
                "[b0] 魔数未命中：guest RAM 中无 \"DSAB0BASECALIBv1!\""
                "（全扫可读 %llu/%llu MiB；集合快扫可读 %llu 页——whole 模式不跑"
                "快扫，恒 0）。两种可能：\n"
                "     ① 魔数没写进 guest——查 replay_run.sh 制脏段的 .b0magic WARN"
                "（aenv exec sh -c 在 guest 内失败会被静默吞掉）；\n"
                "     ② RAM 基址定位错（anonmap 大小匹配推断错）——nonce 段组的"
                "布局错位已被 .b0loc 门禁先行拦截（bitmap 路先于魔数校验，走不到"
                "这里），此处 ② 主要适用于 anonmap 降级模式；对照 /proc/%d/maps"
                "人工核对。\n",
                (unsigned long long)(g_scan_full_bytes >> 20),
                (unsigned long long)((g_lin_pages * PAGE_SIZE) >> 20),
                (unsigned long long)g_scan_ok_pages, (int)g_fc_pid);
            if (g_scan_ok_pages == 0 && g_scan_full_bytes == 0)
                fprintf(stderr, "     （注意：0 页可读——/proc/<pid>/mem 读取本身失败，"
                                "先查权限/ptrace，再怀疑魔数与基址）\n");
            return 1;
        }
        fprintf(stderr, "[b0] 魔数命中 @线性偏移 0x%llx → 读到的确是目标 fc 的 guest RAM"
                        "（写入哨兵 OK；基址/布局正确性以 .b0loc 校验为准，见 GO 行 loc=）\n",
                (unsigned long long)magic_off);
    }

    /* 段式确定性采样：随机段起点 × 连续 SEG_PAGES 页（见文件头「采样口径」） */
    uint64_t target = n_all;
    if (subset_mb > 0) {
        uint64_t want = ((uint64_t)subset_mb << 20) / PAGE_SIZE;
        if (target > want) target = want;
    }
    uint64_t sd = (uint64_t)seed ? (uint64_t)seed : 1;
    if (target < n_all) {
        uint64_t *srcbuf = malloc(n_all * sizeof(uint64_t));
        if (!srcbuf) { printf("B0=NOGO reason=alloc-sample\n"); return 1; }
        memcpy(srcbuf, all, n_all * sizeof(uint64_t));
        uint64_t seg = SEG_PAGES;
        if (seg > n_all) seg = n_all;
        uint64_t filled = 0;
        while (filled < target) {
            uint64_t start = xr(&sd) % (n_all - seg + 1);
            for (uint64_t k = 0; k < seg && filled < target; k++)
                all[filled++] = srcbuf[start + k];
        }
        free(srcbuf);
    }
    qsort(all, target, sizeof(uint64_t), cmp_u64);

    FILE *df = fopen(dump_path, "w");
    if (!df) { fprintf(stderr, "dump %s: %s\n", dump_path, strerror(errno)); return 1; }
    for (uint64_t i = 0; i < target; i++)
        fprintf(df, "%llx\n", (unsigned long long)all[i]);
    if (g_nrsegs > 0)                     /* 映射模型传给臂进程（do_arm 重排 g_rsegs） */
        fprintf(df, "#B0ORDER %s\n", order_name(g_b0_order));
    fclose(df);

    if (g_ram_mode == RAM_MEMFD)
        printf("B0=GO ram_src=memfd ram_mib=%llu segs=%d kvmvm=%d name=%s\n",
               (unsigned long long)(g_ram_limit >> 20), g_nsegs, g_vmfd >= 0, g_memfd_name);
    else if (g_nonce_len == 16) {  /* 路 0：nonce 内容定位（anon 全套 + 段组实测） */
        /* loc = .b0loc 端到端校验命中（gpa→off→hva→内容全链路）；n/a = whole
           退化（全页集合无映射校验力，但 foff 序拼接不重不漏、页集合与排列
           无关）。s0 仅在布局求解成功（loc 有值）时才有意义。 */
        char locs[24], s0s[40];
        if (g_loc_hits >= 0) {
            snprintf(locs, sizeof locs, "%d/%d", g_loc_hits, B0LOC_PAGES);
            snprintf(s0s, sizeof s0s, " s0=%lluMiB", (unsigned long long)(g_slot0_bytes >> 20));
        } else {
            snprintf(locs, sizeof locs, "n/a");
            s0s[0] = 0;
        }
        printf("B0=GO ram_src=nonce ram_mib=%llu segs=%d order=%s file=%s "
               "kvmvm=%d magic=hit@0x%llx loc=%s%s\n",
               (unsigned long long)(g_ram_limit >> 20), g_nrsegs, order_name(g_b0_order),
               g_ram_file[0] ? g_ram_file : "(anon)", g_vmfd >= 0,
               (unsigned long long)magic_off, locs, s0s);
    }
    else
        printf("B0=GO ram_src=anonmap ram_mib=%llu base0=0x%llx base1=0x%llx kvmvm=%d magic=hit@0x%llx\n",
               (unsigned long long)(g_ram_limit >> 20),
               (unsigned long long)g_anon_base[0], (unsigned long long)g_anon_base[1],
               g_vmfd >= 0, (unsigned long long)magic_off);
    printf("DIRTYSRC=%s pages=%llu mb=%.1f\n", src,
           (unsigned long long)target, (double)target * PAGE_SIZE / 1048576.0);
    fprintf(stderr, "[probe] dump %llu 页 → %s（src=%s seed=%ld subset=%ldMB）\n",
            (unsigned long long)target, dump_path, src, seed, subset_mb);
    return 0;
}

/* ---------------- arm 模式 ---------------- */

static int do_arm(const char *arm, const char *pages_path, const char *src_tag,
                  int repeat, int cold, uint32_t pvm_chunk_kb, uint32_t batch_n,
                  int dsa_wait_idle)
{
    if (discover_and_setup(0) != 0) return 1;
    load_pages(pages_path);
    if (g_noffs == 0) { fprintf(stderr, "页集合为空：%s\n", pages_path); return 1; }
    /* dump 尾 #B0ORDER = probe 实测求解的段组排列。臂进程本地 find_by_nonce 的
       排列未必相同（匿名组本地 ASC、实测可能 DESC）→ 按 dump 重排 g_rsegs。
       set_seg_order 幂等检查安全：g_b0_order 恒等于当前实际排列（load_pages
       只写 g_dump_order 不碰 g_b0_order），同值 = 已就位无需重排。 */
    if (g_dump_order >= 0 && g_nrsegs > 0) {
        set_seg_order(g_dump_order);
        fprintf(stderr, "[arm=%s] 段组排列 = %s（dump 传递的 probe 求解模型）\n",
                arm, order_name(g_b0_order));
    }
    if (!strcmp(arm, "dsa") && dsa_setup(batch_n) != 0) return 3;

    /* 4KB 对齐（同上：batch 子任务地址必须页对齐，否则 DSA 报 25） */
    if (posix_memalign((void **)&g_dst, 4096,
                       (size_t)g_noffs * (size_t)PAGE_SIZE) != 0) {
        fprintf(stderr, "posix_memalign dst 失败（%llu 页）\n", (unsigned long long)g_noffs);
        return 1;
    }
    /* dst 预热（计时窗口外，三臂统一口径）：posix_memalign 只保证 4KB 对齐，
       页仍是 lazy 分配，须先 touch 建立物理映射（DSA 访问未映射页同样报 25）。
       预热不保 LLC 常驻——cold=1 的 flush_llc 照常驱逐，冷 cache 口径不受影响。 */
    memset(g_dst, 0, (size_t)g_noffs * (size_t)PAGE_SIZE);

    /* anon 模式：mmap/dsa 臂先一次性预取到本地缓冲（计时窗口外，语义见
       prefetch_lbuf 注释；pvm 臂本身就在测跨进程读，不预取）。 */
    if (g_ram_mode == RAM_ANON && strcmp(arm, "pvm") != 0) {
        if (prefetch_lbuf() != 0) return 1;
        fprintf(stderr, "[arm=%s] anon 模式：源 = 本地预取缓冲 lbuf（rep 计时只含搬运）\n", arm);
    }

    /* src_tag 去逗号（CSV 安全） */
    char tag[128];
    snprintf(tag, sizeof tag, "%s", src_tag);
    for (char *p = tag; *p; p++) if (*p == ',') *p = ';';

    uint32_t chunk_col = strcmp(arm, "pvm") == 0 ? pvm_chunk_kb : 0;
    double bytes = (double)g_noffs * (double)PAGE_SIZE;
    fprintf(stderr, "[arm=%s] pages=%llu (%.1fMB) repeat=%d cold=%d src=%s\n",
            arm, (unsigned long long)g_noffs, bytes / 1048576.0, repeat, cold, tag);

    for (int irep = 1; irep <= repeat; irep++) {
        if (cold) flush_llc();
        g_note[0] = 0;
        double c0 = cpu_sec(), w0 = wall_sec();
        int rc = run_arm_dispatch(arm, pvm_chunk_kb, dsa_wait_idle);
        double c1 = cpu_sec(), w1 = wall_sec();
        if (rc != 0) return strcmp(arm, "dsa") == 0 ? 3 : 1;
        if (!verify_dst(arm)) {
            fprintf(stderr, "checksum 不一致 arm=%s irep=%d —— 中止（数据完整性优先）\n", arm, irep);
            return 2;
        }
        double cpu_ms = (c1 - c0) * 1e3;
        double wall_ms = (w1 - w0) * 1e3;
        double per_gb = bytes > 0 ? cpu_ms / (bytes / 1e9) : 0.0;
        double util = wall_ms > 0 ? 100.0 * cpu_ms / wall_ms : 0.0;
        for (char *p = g_note; *p; p++) if (*p == ',') *p = ';';
        printf("%s,%s,%llu,%.1f,%u,%d,%d,%.2f,%.2f,%.2f,%.1f,1,%s\n",
               arm, tag, (unsigned long long)g_noffs, bytes / 1048576.0,
               chunk_col, cold, irep, cpu_ms, wall_ms, per_gb, util, g_note);
        fflush(stdout);
    }
    dsa_teardown();
    return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *arm = NULL, *pages_path = NULL, *dump_path = NULL, *src_tag = "raw";
    int probe = 0, cold = 1, repeat = 5, seed = 1, dsa_wait_idle = 1;
    /* dsa_wait_idle 默认 1（= DML UMWAIT）：本程序输出 cpu_ms 的口径是
       CLOCK_PROCESS_CPUTIME_ID，BUSY_POLL 等待会自旋烧 CPU 使 dsa 臂 cpu_ms
       虚高，反映不出「释放了多少 CPU」。默认取 idle，保证直接调用本程序
       （不经 replay_run.sh）也得到可比 onCPU 数；显式 --dsa-wait busy 才切回
       自旋，且此时 note 列打 spin=1 标记。 */
    long fc_pid = 0, subset_mb = 256, pvm_chunk_kb = 64, batch_n = 48, guest_mib = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--fc-pid") && i + 1 < argc) fc_pid = atol(argv[++i]);
        else if (!strcmp(a, "--probe")) probe = 1;
        else if (!strcmp(a, "--pages") && i + 1 < argc) pages_path = argv[++i];
        else if (!strcmp(a, "--dump-pages") && i + 1 < argc) dump_path = argv[++i];
        else if (!strcmp(a, "--arm") && i + 1 < argc) arm = argv[++i];
        else if (!strcmp(a, "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(a, "--cold") && i + 1 < argc) cold = atoi(argv[++i]);
        else if (!strcmp(a, "--subset-mb") && i + 1 < argc) subset_mb = atol(argv[++i]);
        else if (!strcmp(a, "--seed") && i + 1 < argc) seed = atoi(argv[++i]);
        else if (!strcmp(a, "--pvm-chunk-kb") && i + 1 < argc) pvm_chunk_kb = atol(argv[++i]);
        else if (!strcmp(a, "--batch-n") && i + 1 < argc) batch_n = atol(argv[++i]);
        else if (!strcmp(a, "--dsa-wait") && i + 1 < argc) dsa_wait_idle = !strcmp(argv[++i], "idle");
        else if (!strcmp(a, "--src-tag") && i + 1 < argc) src_tag = argv[++i];
        else if (!strcmp(a, "--guest-mib") && i + 1 < argc) guest_mib = atol(argv[++i]);
        else if (!strcmp(a, "--nonce-hex") && i + 1 < argc) {
            const char *h = argv[++i];
            if (strlen(h) != 32) {
                fprintf(stderr, "--nonce-hex 须为 32 个 hex 字符（16B 随机数）\n");
                return 1;
            }
            for (int k = 0; k < 16; k++) {
                unsigned b;
                if (sscanf(h + 2 * k, "%2x", &b) != 1) {
                    fprintf(stderr, "--nonce-hex 含非 hex 字符：%s\n", h);
                    return 1;
                }
                g_nonce[k] = (uint8_t)b;
            }
            g_nonce_len = 16;
        }
        else { usage(argv[0]); return 1; }
    }
    /* nonce 提供时可自动定位 fc 进程（--fc-pid 仅作优先扫描提示）；否则必填 */
    if (fc_pid <= 0 && g_nonce_len != 16) { usage(argv[0]); return 1; }
    g_fc_pid = (pid_t)fc_pid;
    if (subset_mb < 0 || pvm_chunk_kb < 4 || batch_n < 1) { usage(argv[0]); return 1; }
    if (guest_mib < 0 || guest_mib > 64 * 1024) { usage(argv[0]); return 1; }
    g_ram_mib = guest_mib;
    if (repeat < 1 || repeat > MAX_REPEAT) {
        fprintf(stderr, "--repeat 须在 [1,%d]\n", MAX_REPEAT);
        return 1;
    }

    if (probe) {
        if (arm || pages_path) { fprintf(stderr, "--probe 与 --arm 互斥\n"); return 1; }
        if (!dump_path) { fprintf(stderr, "probe 需要 --dump-pages <file>\n"); return 1; }
        return do_probe(dump_path, subset_mb, seed);
    }

    if (!arm || !pages_path) { usage(argv[0]); return 1; }
    if (strcmp(arm, "pvm") && strcmp(arm, "mmap") && strcmp(arm, "dsa")) {
        fprintf(stderr, "--arm 须为 pvm|mmap|dsa\n");
        return 1;
    }
    return do_arm(arm, pages_path, src_tag, repeat, cold,
                  (uint32_t)pvm_chunk_kb, (uint32_t)batch_n, dsa_wait_idle);
}
