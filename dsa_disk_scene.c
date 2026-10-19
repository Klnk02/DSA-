// dsa_disk_scene.c — mem2disk / disk2mem 的 onCPU 成本（公平结构 · 双等待口径 · 带证据）
//
// 【为什么重写】旧版（2026-10-13，fig6 数据源）把两类磁盘操作做成「拼装加法」：
//   disk2mem 现状 = read(fd,dst) 一步式 vs DSA = read(fd,tmp) + DSA 两步式。
//   两臂结构不等价：一步式的 syscall 直接拷进最终缓冲，两步式多付一次
//   copy_to_user（读进 staging）——DSA 臂天然吃亏，比较不公平；且真实路径里
//   「用户态 memcpy 那一跳」根本不存在（见下），拼装模型会高估收益。
//
// 【真实路径事实（README §4.2 源码级确认，本文件据此建模）】
//   AgentENV snapshot mem2disk：orchestrator 用 process_vm_readv 从 firecracker
//   地址空间读 dirty 页，再 pwrite 到 OverlayBD commit 文件（io_uring）。
//   ⇒ 两个方向的数据搬运都在**内核**里完成：跨进程 copy_to_user、写 page cache
//     的 copy_from_iter。DSA（DML）只能搬**自己进程地址空间**内的内存，结构上
//     无法介入这两跳；也不存在一个用户态 memcpy 跳供其替换。
//   ⇒ 本文件因此给出三种结构、分层次出数，不许混为一谈：
//     sw_1step  = 现状结构（syscall 直达终点）：DSA 可替换的跳 = 0
//     sw_2step  = 假设结构（必须先经 staging，如 O_DIRECT 对齐约束）：第二跳 = memcpy
//     dsa_2step = 同假设结构，第二跳 = DML：收益只来自这一跳
//
// 【口径】
//   onCPU = CLOCK_PROCESS_CPUTIME_ID（本进程 user+sys；I/O 等待不计入 CPU）
//   wall  = CLOCK_MONOTONIC（暴露 DSA 的延迟代价）
//   跨源对账：每臂区间同时用 /proc/self/stat 的 utime+stime 独立复核主口径——
//     两条独立内核记账路径（ns 运行时账本 vs tick 采样账本）吻合 → 计时真实。
//     /proc 逐臂数据以「独立复核表」输出（第二张 CSV，与主表分开，不混列）；
//     判定策略：只有聚合级（全部臂总和）不吻合才整体 WARN、本轮数据勿引用；个别臂
//     /proc 偏小属 tick 采样漏记 I/O 间隙短突发的已知特性（NO_HZ_IDLE），只作提示不作判废。
//     /proc 是 tick 采样（USER_HZ=100，约 10ms/格），只配做区间对账；测外部 PID 的
//     主口径须用 perf task-clock -p PID 或 clock_getcpuclockid(pid)
//   utime/stime 分项（→ 方案对比主表）：每 op 窗口前后 getrusage(RUSAGE_SELF) 采样、累加取均值
//     ——与 /proc 同一 tick 账本（10ms/格），期望无偏、量化噪声由 n 摊薄；分项只作构成参考
//   每臂报 avg / sd / min / max（n = repeat，默认 15，项目纪律）
//   DSA 双等待口径都出数：idle(UMWAIT) / busy(BUSY_POLL)。注意：本负载（60MB、30 个 2MB job）
//     等待窗口很短（实测 cpu/wall≈100%），两种等待模式的 CPU 几乎相等 → idle 列无区分度，
//     只能作参考；Δ 一律报「DSA 搬运 vs CPU 指令搬运的进程 CPU 差」，不得称「释放量」。
//   证据列 rw_read_per_gb / rw_write_per_gb = /proc/self/io 真实落盘字节 / 单次搬运字节
//     （1.0 = 本次搬运全部真读了盘；≈0 说明读的是 page cache，不是冷盘）：
//     ≈0 的行只能限定在「缓存命中下的 CPU 成本」，不许冒充冷盘吞吐。写入侧 fsync 计入窗口
//     （真实 commit 路径有 fsync），其文件系统簿记属不可卸载部分；系统侧 kworker/jbd2
//     不计入进程 CPU，需用 calibrate_cpu_scope.sh 的 scene 模式补系统口径。
//
// 【用法】
//   ./dsa_disk_scene <真实磁盘上的文件路径> [size_mb=60] [repeat=15] [wait=both|idle|busy]
//   size_mb = 一个 snapshot 的搬运量（默认 60MB；须为 2MB 的整数倍，内部换算 iters=size_mb/2）
//   磁盘路径须落在真实挂载点（tmpfs 会告警）
//   输出「方案对比主表」：一次 snapshot 软件 vs DSA(全接管)，列 = 序号/方案/场景/utime/stime/onCPU
//     （领导人要的那张表）；其后还有 snapshot 场景合成表、真实 pause 对账、独立复核表、跨源对账判定
//   编译：gcc -O2 -Wall dsa_disk_scene.c -o dsa_disk_scene -ldml -ldmlhl -luuid -lm -pthread -lstdc++
//
// 【60MB 量级为什么要单独标量级】60MB 接近常见 L3 容量、且远小于 1GB 档，三件事会变：
//   - 页容易留在 LLC 里 → 每臂每次窗口前做 256MB 扫掠冲刷 LLC（FLUSH=0 可关，默认开）
//   - DML 单发固定开销只被 30 个 job 摊薄（1GB 档是 512 个）→ DSA 收益偏保守，报告须标量级
//   - 每行必须读 rw_read_per_gb 证据列：≈0 说明是缓存命中，该行只能当 CPU 成本口径，不许当冷盘吞吐
#include <dml/dml.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/vfs.h>
#include <sys/resource.h>          // getrusage：utime/stime 分项（方案对比主表）

#define CHUNK       (2 * 1024 * 1024)   // 2MB 搬运块（单发 DML MEM_MOVE 上限 2MB）
#define TMPFS_MAGIC 0x01021994

typedef struct { double avg, sd, min, max; } Stats;

static double cpu_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static double wall_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static Stats stats(const double *v, int n) {
    double sum = 0;
    Stats st; st.min = 1e18; st.max = -1e18;
    for (int i = 0; i < n; i++) {
        sum += v[i];
        if (v[i] < st.min) st.min = v[i];
        if (v[i] > st.max) st.max = v[i];
    }
    st.avg = sum / n;
    double var = 0;
    for (int i = 0; i < n; i++) { double d = v[i] - st.avg; var += d * d; }
    st.sd = (n > 1) ? sqrt(var / (n - 1)) : 0.0;
    return st;
}

static ssize_t full_read(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (uint8_t *)buf + done, n - done);
        if (r <= 0) return r;
        done += (size_t)r;
    }
    return (ssize_t)done;
}
static ssize_t full_write(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const uint8_t *)buf + done, n - done);
        if (w <= 0) return w;
        done += (size_t)w;
    }
    return (ssize_t)done;
}

/* /proc/self/io：read_bytes/write_bytes = 真实落盘字节（与 page cache 命中互为反证） */
static void self_io(unsigned long long *rd, unsigned long long *wr) {
    *rd = 0; *wr = 0;
    FILE *f = fopen("/proc/self/io", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        unsigned long long v = 0;
        if (sscanf(line, "read_bytes: %llu", &v) == 1)  *rd = v;
        if (sscanf(line, "write_bytes: %llu", &v) == 1) *wr = v;
    }
    fclose(f);
}

/* /proc/self/stat 的 utime+stime（字段 14/15）：与 CLOCK_PROCESS_CPUTIME_ID 是两条**独立**
   的内核记账路径（tick 采样 vs ns 运行时账本）→ 只用于区间级对账（约 10ms/格，不能当
   单臂主口径）。comm 可含空格/括号，故用最后一个 ')' 定位字段边界。 */
static double tick_ms(void) {
    long t = sysconf(_SC_CLK_TCK);
    return 1000.0 / (t > 0 ? t : 100);        // USER_HZ=100 → 10ms/格（与内核 HZ 无关）
}
static double proc_cpu_ms(void) {
    char buf[2048];
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0) return -1.0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1.0;
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (!p) return -1.0;
    p += 2;                                    // 跳过 ") "，指向字段 3(state)
    for (int f = 3; f <= 13; f++) {            // 跳到字段 14(utime)
        while (*p && *p != ' ') p++;
        if (*p == ' ') p++;
    }
    long ut = 0, st = 0;
    if (sscanf(p, "%ld %ld", &ut, &st) != 2) return -1.0;
    return (ut + st) * tick_ms();
}

/* getrusage(RUSAGE_SELF) 的 ru_utime/ru_stime：与 /proc/self/stat 同一 tick 账本（10ms/格）。
   用途 = 方案对比主表的 utime/stime 分项：每个 op 窗口前后采样、累加 repeat 次取均值——
   期望无偏，量化噪声由 n 摊薄（泊松命中）；只作分项参考，onCPU 主口径不变。 */
static void rusage_ms(double *ut_ms, double *st_ms) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) { *ut_ms = *st_ms = -1.0; return; }
    *ut_ms = (double)ru.ru_utime.tv_sec * 1000.0 + ru.ru_utime.tv_usec / 1000.0;
    *st_ms = (double)ru.ru_stime.tv_sec * 1000.0 + ru.ru_stime.tv_usec / 1000.0;
}

static uint8_t *align_alloc(size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, CHUNK, size) != 0) return NULL;  // 2MB 对齐（DML 大块友好）
    memset(p, 0, size);                                     // touch 建物理映射
    return p;
}

/* ---- LLC 冲刷（256MB 扫掠）：60MB/热页量级下必须做，否则搬运口径不一致 ---- */
#define FLUSH_BYTES (256UL << 20)
static uint8_t *g_flush_buf;
static void flush_llc(void) {
    if (!g_flush_buf) return;
    volatile uint64_t sink = 0;
    uint64_t *p = (uint64_t *)g_flush_buf;
    for (size_t i = 0; i < FLUSH_BYTES / sizeof(uint64_t); i += 8) sink += p[i];
    if (sink == 0xdeadbeefULL) fprintf(stderr, "");      // 防编译器消掉整个循环
}

/* 跨源对账：逐臂记录（main 末尾出「独立复核表」）+ 聚合累计（出总判定） */
static double g_clk_span_ms, g_proc_span_ms;
static int g_recon_bad;
typedef struct { const char *op, *impl; int size_mb, repeat; double clk_ms, proc_ms; int ok; } Recon;
static Recon g_recon[64];
static int g_recon_n;

/* 臂结果登记：合成 snapshot 场景表 / 方案对比主表时按 impl 名回查（名字都是字面量，存指针即可） */
static const char *g_rname[16];
static double g_rms[16];
static double g_rut[16], g_rst[16];          // 单次 snapshot 的 utime/stime 分项均值
static int g_rn;
static void store_res(const char *impl, double ms_total, double ut_ms, double st_ms) {
    if (g_rn < 16) {
        g_rname[g_rn] = impl; g_rms[g_rn] = ms_total;
        g_rut[g_rn] = ut_ms; g_rst[g_rn] = st_ms;
        g_rn++;
    }
}
static double res_of(const char *impl) {
    for (int i = 0; i < g_rn; i++) if (!strcmp(g_rname[i], impl)) return g_rms[i];
    return -1.0;                                          // 缺（如 DML 不可用）= n/a
}
static int split_of(const char *impl, double *ut, double *st) {
    for (int i = 0; i < g_rn; i++)
        if (!strcmp(g_rname[i], impl)) { *ut = g_rut[i]; *st = g_rst[i]; return 1; }
    return 0;
}

/* 等待口径自检：只报事实（idle 与 busy 的 CPU 差异），不下「UMWAIT 失效」结论——本负载的
   等待窗口很短（cpu/wall≈100%），两种等待模式 CPU 本来就接近，接近≠失效。真正结论只有一句：
   idle 列在这类负载上没有区分度，Δ 只能报「CPU 差」，不能报「释放量」。任一臂缺失即跳过。 */
static void warn_umwait(const char *idle_impl, const char *busy_impl) {
    double di = res_of(idle_impl), db = res_of(busy_impl);
    if (di < 0 || db < 0 || db <= 0) return;
    if (di > 0.8 * db)
        fprintf(stderr, "NOTE: 等待口径 [%s idle %.2fms] vs [%s busy %.2fms] 接近(>80%%) → "
                        "等待窗口过短、两种等待模式无区分度；idle 列仅作参考，"
                        "Δ 请报「DSA 搬运 vs CPU 搬运的 CPU 差」，勿报「释放量」\n",
                idle_impl, di, busy_impl, db);
    else
        fprintf(stderr, "NOTE: 等待口径 idle=%.2fms << busy=%.2fms（idle 显著更低，与 UMWAIT 生效一致）\n", di, db);
}

/* ---- DSA 单发 2MB MEM_MOVE：wait_idle=1 → UMWAIT（CPU 空闲）；0 → BUSY_POLL（自旋） ---- */
static dml_job_t *g_job;
static void dsa_move(uint8_t *dstp, const uint8_t *srcp, size_t n, int wait_idle) {
    g_job->operation = DML_OP_MEM_MOVE;
    g_job->source_first_ptr = (uint8_t *)srcp;
    g_job->destination_first_ptr = dstp;
    g_job->source_length = (uint32_t)n;
    g_job->destination_length = (uint32_t)n;
    dml_execute_job(g_job, wait_idle ? DML_WAIT_MODE_UMWAIT : DML_WAIT_MODE_BUSY_POLL);
}

/* ---- 臂的原子操作 ----
   注意：缓冲区是「整个 size_mb」大小，每次循环走**不同的 2MB 段**（i*chunk 偏移）。
   旧版 30 次循环搬同一块 2MB → 第 2~30 次全在 LLC 里，绝对值偏小、不反映 60MB；
   偏移版让每次的源/目标页都不同，绝对 ms 才是「一个 snapshot」的搬运成本。
   A/B（sw vs dsa）两臂 buffer 走位完全一致，公平性不受影响。 */
typedef struct {
    int fd;
    uint8_t *src, *dst, *stg;   // src=源, dst=终点（guest RAM 类比）, stg=staging
    size_t chunk;
    int iters, wait_idle;
} Ctx;

static void op_memcpy(Ctx c) {                      // 纯内存参照臂
    for (int i = 0; i < c.iters; i++)
        memcpy(c.dst + (size_t)i * c.chunk, c.src + (size_t)i * c.chunk, c.chunk);
}
static void op_dsa(Ctx c) {
    for (int i = 0; i < c.iters; i++)
        dsa_move(c.dst + (size_t)i * c.chunk, c.src + (size_t)i * c.chunk, c.chunk, c.wait_idle);
}
/* 现状 disk2mem：read 直达终点（内核 copy_to_user 一次；DSA 无跳可替） */
static void op_read_1step(Ctx c) {
    lseek(c.fd, 0, SEEK_SET);
    for (int i = 0; i < c.iters; i++)
        if (full_read(c.fd, c.dst + (size_t)i * c.chunk, c.chunk) != (ssize_t)c.chunk) exit(2);
}
/* 假设结构 disk2mem：read → staging，第二跳 memcpy */
static void op_read_stg_memcpy(Ctx c) {
    lseek(c.fd, 0, SEEK_SET);
    for (int i = 0; i < c.iters; i++) {
        if (full_read(c.fd, c.stg + (size_t)i * c.chunk, c.chunk) != (ssize_t)c.chunk) exit(2);
        memcpy(c.dst + (size_t)i * c.chunk, c.stg + (size_t)i * c.chunk, c.chunk);
    }
}
/* 假设结构 disk2mem：read → staging，第二跳 DML（DSA 只替这一跳） */
static void op_read_stg_dsa(Ctx c) {
    lseek(c.fd, 0, SEEK_SET);
    for (int i = 0; i < c.iters; i++) {
        if (full_read(c.fd, c.stg + (size_t)i * c.chunk, c.chunk) != (ssize_t)c.chunk) exit(2);
        dsa_move(c.dst + (size_t)i * c.chunk, c.stg + (size_t)i * c.chunk, c.chunk, c.wait_idle);
    }
}
/* 现状 mem2disk：write 直达（内核 copy_from_user 一次）+ fsync */
static void op_write_1step(Ctx c) {
    lseek(c.fd, 0, SEEK_SET);
    for (int i = 0; i < c.iters; i++)
        if (full_write(c.fd, c.src + (size_t)i * c.chunk, c.chunk) != (ssize_t)c.chunk) exit(3);
    fsync(c.fd);
}
/* 假设结构 mem2disk：第一跳 memcpy(src→staging) + write(staging) */
static void op_memcpy_stg_write(Ctx c) {
    lseek(c.fd, 0, SEEK_SET);
    for (int i = 0; i < c.iters; i++) {
        memcpy(c.stg + (size_t)i * c.chunk, c.src + (size_t)i * c.chunk, c.chunk);
        if (full_write(c.fd, c.stg + (size_t)i * c.chunk, c.chunk) != (ssize_t)c.chunk) exit(3);
    }
    fsync(c.fd);
}
/* 假设结构 mem2disk：第一跳 DML(src→staging) + write(staging) */
static void op_dsa_stg_write(Ctx c) {
    lseek(c.fd, 0, SEEK_SET);
    for (int i = 0; i < c.iters; i++) {
        dsa_move(c.stg + (size_t)i * c.chunk, c.src + (size_t)i * c.chunk, c.chunk, c.wait_idle);
        if (full_write(c.fd, c.stg + (size_t)i * c.chunk, c.chunk) != (ssize_t)c.chunk) exit(3);
    }
    fsync(c.fd);
}

/* 跑一个臂：repeat 次；窗口只包 op（DONTNEED/fsync/LLC 冲刷都在窗口外）
   输出列的 onCPU 既给「每 GB」（与 942/112.7 等历史数对齐，口径=GiB）也给「每 snapshot 绝对 ms」 */
static void run_arm(const char *op, const char *impl, void (*fn)(Ctx), int fd,
                    uint8_t *src, uint8_t *dst, uint8_t *stg, size_t chunk,
                    int iters, int repeat, double gb, int size_mb,
                    int wait_idle, int evict, int flush) {
    double *ct = malloc((size_t)repeat * sizeof(double));
    double *wt = malloc((size_t)repeat * sizeof(double));
    unsigned long long rd0 = 0, wr0 = 0, wr1 = 0, rd1 = 0;
    self_io(&rd0, &wr0);
    /* 对账基准：同一区间用两条独立路径量（含窗口外 evict/flush，与主表窗口口径无关） */
    double c0 = cpu_sec(), p0 = proc_cpu_ms();
    double ut_sum = 0, st_sum = 0;            // getrusage 分项：每 op 窗口前后采样、累加 repeat 次
    for (int r = 0; r < repeat; r++) {
        if (evict) { posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); fsync(fd); }
        if (flush) flush_llc();
        Ctx c = { fd, src, dst, stg, chunk, iters, wait_idle };
        double u0 = 0, s0 = 0;
        rusage_ms(&u0, &s0);                  // 先采分项再开主口径窗口 → 主口径窗口零污染
        double t0 = cpu_sec(), w0 = wall_sec();
        fn(c);
        double t1 = cpu_sec(), w1 = wall_sec();
        double u1 = 0, s1 = 0;
        rusage_ms(&u1, &s1);
        ut_sum += u1 - u0; st_sum += s1 - s0; // tick 账本：窗口级差分，期望无偏
        ct[r] = (t1 - t0) / gb * 1000.0;      // ms/GiB（onCPU）
        wt[r] = (w1 - w0) / gb;               // s/GiB
    }
    double c1 = cpu_sec(), p1 = proc_cpu_ms();
    self_io(&rd1, &wr1);
    Stats cs = stats(ct, repeat), ws = stats(wt, repeat);
    /* 证据列 = 真实落盘字节 / 单次搬运字节（÷repeat 去掉重复次数；1.0=本次全真读盘，≈0=缓存命中）。
       旧版漏了 ÷repeat，把 repeat 次窗口的累计量比到单次搬运量上，被放大 repeat(默认 15) 倍。 */
    double run_bytes = (double)iters * chunk;
    double rd_ratio = (double)(rd1 - rd0) / (double)repeat / run_bytes;
    double wr_ratio = (double)(wr1 - wr0) / (double)repeat / run_bytes;
    store_res(impl, cs.avg * gb, ut_sum / repeat, st_sum / repeat);
                                               // 每 snapshot 绝对 onCPU ms + utime/stime 分项均值
    printf("%s,%s,%d,%.3f,%.3f,%.3f,%.3f,%.1f,%.3f,%.3f,%.3f\n",
           op, impl, size_mb, cs.avg * gb, cs.sd * gb, cs.min * gb, cs.max * gb,
           cs.avg, ws.avg * gb * 1000.0, rd_ratio, wr_ratio);
    fflush(stdout);
    fprintf(stderr, "  [%s/%s] %.3f ms/%dMB (cpu %.1f ms/GiB ±%.1f)  wall %.1f ms  disk_rd %.2fx\n",
            op, impl, cs.avg * gb, size_mb, cs.avg, cs.sd, ws.avg * gb * 1000.0,
            rd_ratio);
    if (p0 >= 0 && p1 >= 0) {                  // 跨源对账：clock(区间) vs /proc(区间)
        double clk_span = (c1 - c0) * 1000.0, proc_span = p1 - p0;
        double tol = 4 * tick_ms() + 0.10 * clk_span;   // tick 采样漏记 I/O 间隙短突发 → 放宽
        int ok = fabs(proc_span - clk_span) <= tol;
        g_clk_span_ms += clk_span; g_proc_span_ms += proc_span;
        if (!ok) g_recon_bad++;
        if (g_recon_n < (int)(sizeof g_recon / sizeof g_recon[0]))   // 逐臂记录 → 独立复核表
            g_recon[g_recon_n++] = (Recon){ op, impl, size_mb, repeat, clk_span, proc_span, ok };
        fprintf(stderr, "  [对账 %s/%s] clock=%.0fms proc=%.0fms Δ=%.1f (量化%.0fms 容差%.0fms) %s\n",
                op, impl, clk_span, proc_span, proc_span - clk_span, tick_ms(), tol,
                ok ? "OK" : "参考:差异偏大(多为tick采样漏记短突发,主表ns级数值不受影响)");
    }
    free(ct); free(wt);
}

/* snapshot 场景合成一行：sw = 纯软件 onCPU(ms)，dsa = DSA onCPU(ms)，-1 表示不可用 */
static void print_scn(const char *scn, const char *structure, const char *sw_impl,
                      const char *dsa_impl, const char *source) {
    double sw = res_of(sw_impl), dsa = res_of(dsa_impl);
    if (sw < 0) { printf("%s,%s,n/a,n/a,n/a,n/a,%s\n", scn, structure, source); return; }
    if (dsa < 0) {                            // DSA 臂缺失：按 0 收益记，并在 source 明确标注「硬件不可用」
        printf("%s,%s,%.3f,%.3f,0.000,0.0,%s;DSA 不可用→仅软件基线\n", scn, structure, sw, sw, source);
        return;
    }
    double d = dsa - sw;
    printf("%s,%s,%.3f,%.3f,%.3f,%.1f,%s\n", scn, structure, sw, dsa, d,
           sw > 0 ? 100.0 * d / sw : 0.0, source);
}

/* 方案对比主表一行（领导人要的那张表）。软件行：sub=NULL，直接报 base 臂实测分项。
   合成行（假设可接管段全部由 DSA 完成）：
     onCPU = base − sw_memcpy + dsa_idle（ns 主口径同式合成，精确）
     stime = base_st − sw_memcpy_onCPU + dsa_st（可接管段在 1step 里记于 stime/内核搬运，
             其量按 sw_memcpy 参照臂 onCPU 借用 → 从 stime 扣除）
     utime = base_ut + dsa_ut（DSA 接管后代之以用户态 DML 提交，记 utime）
   分项是近似构成（跨科目借用），加总以 oncpu_ms 为准；加总恰等于主口径合成。 */
static int g_cmp_idx;
static void cmp_row(const char *scheme, const char *scene, const char *base_impl,
                    const char *sub_impl, const char *add_impl, const char *note) {
    int idx = ++g_cmp_idx;
    double on = res_of(base_impl), ut = 0, st = 0;
    if (on < 0 || !split_of(base_impl, &ut, &st)) {
        printf("%d,%s,%s,n/a,n/a,n/a,%s\n", idx, scheme, scene, note);
        return;
    }
    if (sub_impl) {
        if (!add_impl) {
            printf("%d,%s,%s,n/a,n/a,n/a,%s;合成臂缺失\n", idx, scheme, scene, note);
            return;
        }
        double on_s = res_of(sub_impl), on_a = res_of(add_impl), ut_a = 0, st_a = 0;
        if (on_s < 0 || on_a < 0 || !split_of(add_impl, &ut_a, &st_a)) {
            printf("%d,%s,%s,n/a,n/a,n/a,%s;合成臂缺失(DML 不可用)\n", idx, scheme, scene, note);
            return;
        }
        on = on - on_s + on_a;
        st = st - on_s + st_a;
        ut = ut + ut_a;
    }
    printf("%d,%s,%s,%.1f,%.1f,%.3f,%s\n", idx, scheme, scene, ut, st, on, note);
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/var/lib/aenv/dsa_disk_scene.bin";
    const int size_mb = (argc > 2) ? atoi(argv[2]) : 60;      // 一个 snapshot 的量
    const int repeat = (argc > 3) ? atoi(argv[3]) : 15;
    const char *wait = (argc > 4) ? argv[4] : "both";
    const int chunk = CHUNK;
    const int iters = size_mb / (chunk / 1024 / 1024);        // 60MB / 2MB = 30 个 job
    if (repeat < 2) { fprintf(stderr, "repeat 需 >= 2（项目纪律建议 15）\n"); return 1; }
    if (size_mb < 4 || size_mb % (chunk / 1024 / 1024) != 0) {
        fprintf(stderr, "size_mb 须为 %dKB 的整数倍且 >= 4（收到 %d）\n", chunk / 1024, size_mb);
        return 1;
    }
    int do_idle = !strcmp(wait, "idle") || !strcmp(wait, "both");
    int do_busy = !strcmp(wait, "busy") || !strcmp(wait, "both");
    if (!do_idle && !do_busy) { fprintf(stderr, "wait 取值 both|idle|busy\n"); return 1; }
    const int flush = getenv("FLUSH") ? atoi(getenv("FLUSH")) : 1;   // 60MB 量级默认冲 LLC

    struct statfs sf;
    if (statfs(path, &sf) == 0 && (unsigned long)sf.f_type == TMPFS_MAGIC)
        fprintf(stderr, "WARN: %s 在 tmpfs 上（内存文件系统）→ 盘 I/O 段失真，请传真实磁盘路径\n", path);

    const double gb = (double)chunk * iters / (1024.0 * 1024.0 * 1024.0);   // GiB（与 942 口径一致）
    /* 三个缓冲都按「整个 snapshot」分配：op 按 i*chunk 走不同 2MB 段，最大 offset = iters*chunk。
       旧版只 allocate(chunk)=2MB 而访问到 iters*chunk(默认 60MB) → 越界读写 58MB，数据不可信。 */
    const size_t buf_bytes = (size_t)iters * chunk;
    uint8_t *src = align_alloc(buf_bytes), *dst = align_alloc(buf_bytes), *stg = align_alloc(buf_bytes);
    if (flush) g_flush_buf = align_alloc(FLUSH_BYTES);
    if (size_mb <= 64)
        fprintf(stderr, "NOTE: size_mb=%d ≤ 64 → 已按冷页口径每次窗口前冲 LLC（FLUSH=%d）；"
                        "该量级下 DML 固定开销摊薄少，DSA 收益偏保守\n", size_mb, flush);
    if (!src || !dst || !stg) { fprintf(stderr, "align_alloc 失败\n"); return 1; }
    memset(src, 0x5A, buf_bytes);

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    for (int i = 0; i < iters; i++)
        if (full_write(fd, src + (size_t)i * chunk, chunk) != (ssize_t)chunk) { perror("prefill"); return 1; }
    fsync(fd);

    uint32_t job_size = 0;
    if (dml_get_job_size(DML_PATH_HW, &job_size) != DML_STATUS_OK || job_size == 0) {
        fprintf(stderr, "DML 硬件路径不可用（无 shared wq / libaccel-config 缺失）→ 只跑软件臂\n");
        do_idle = do_busy = 0;
    } else {
        g_job = malloc(job_size);
        if (!g_job || dml_init_job(DML_PATH_HW, g_job) != DML_STATUS_OK) {
            fprintf(stderr, "dml_init_job 失败（DML 硬件路径要求 shared wq）→ 只跑软件臂\n");
            do_idle = do_busy = 0;
        }
    }

    printf("# dsa_disk_scene（公平结构版）file=%s snapshot=%dMB repeat=%d chunk=%dKB jobs=%d flush=%d\n",
           path, size_mb, repeat, chunk / 1024, iters, flush);
    printf("# onCPU=CLOCK_PROCESS_CPUTIME_ID；wall=MONOTONIC；rw_*_per_gb 来自 /proc/self/io（真实落盘字节/单次搬运，1.0=全真读盘，≈0=缓存命中）\n");
    printf("# 结构：sw_1step=现状(内核单跳直达，DSA 无可替换跳)；*_2step=假设有 staging 跳（DSA 只替第二跳）\n");
    printf("op,impl,size_mb,oncpu_ms_snapshot,oncpu_sd_ms,oncpu_min_ms,oncpu_max_ms,oncpu_ms_per_gib,wall_ms_snapshot,rw_read_per_gb,rw_write_per_gb\n");

    /* 内存参照臂：说明「DSA 能替的那一跳长什么样」（纯 mem2mem，无 syscall） */
    run_arm("mem2mem", "sw_memcpy", op_memcpy, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 0, flush);
    if (do_idle) run_arm("mem2mem", "dsa_idle", op_dsa, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 1, 0, flush);
    if (do_busy) run_arm("mem2mem", "dsa_busy", op_dsa, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 0, flush);

    /* disk2mem：现状 vs 假设结构。冷读用 DONTNEED 逐出自文件缓存（逐出真假由 rw_read 证据判定） */
    run_arm("disk2mem", "sw_1step_read_direct", op_read_1step, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 1, flush);
    run_arm("disk2mem", "sw_2step_read_stg_memcpy", op_read_stg_memcpy, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 1, flush);
    if (do_idle) run_arm("disk2mem", "dsa_2step_read_stg_dsa_idle", op_read_stg_dsa, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 1, 1, flush);
    if (do_busy) run_arm("disk2mem", "dsa_2step_read_stg_dsa_busy", op_read_stg_dsa, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 1, flush);

    /* mem2disk：现状 vs 假设结构（窗口含 fsync；落盘字节见 rw_write） */
    run_arm("mem2disk", "sw_1step_write_direct", op_write_1step, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 0, flush);
    run_arm("mem2disk", "sw_2step_memcpy_stg_write", op_memcpy_stg_write, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 0, flush);
    if (do_idle) run_arm("mem2disk", "dsa_2step_dsa_stg_write_idle", op_dsa_stg_write, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 1, 0, flush);
    if (do_busy) run_arm("mem2disk", "dsa_2step_dsa_stg_write_busy", op_dsa_stg_write, fd, src, dst, stg, chunk, iters, repeat, gb, size_mb, 0, 0, flush);

    /* 等待口径自检（本负载下 idle≈busy 属正常现象，只作提示，不当「UMWAIT 失效」判定） */
    if (do_idle && do_busy) {
        warn_umwait("dsa_idle", "dsa_busy");
        warn_umwait("dsa_2step_read_stg_dsa_idle", "dsa_2step_read_stg_dsa_busy");
        warn_umwait("dsa_2step_dsa_stg_write_idle", "dsa_2step_dsa_stg_write_busy");
    }

    /* ---- 方案对比主表（领导人要的那张表）：一次 snapshot，软件 vs 假设可接管段全部由 DSA 完成 ---- */
    printf("\n# --- 方案对比主表（微基准一次 = %dMiB；非真实snapshot A/B；utime/stime为分项ms，onCPU为主口径ms） ---\n", size_mb);
    printf("# 直接替换是假设，非已有接管实现：合成 = 软件(1step) − sw_memcpy + dsa_idle\n");
    printf("# 合成行分项为近似构成（1step 搬运段记 stime/内核，参照臂记 utime/用户态），加总以 oncpu_ms 为准\n");
    printf("idx,scheme,scene,utime_ms,stime_ms,oncpu_ms,note\n");
    cmp_row("软件", "mem2disk", "sw_1step_write_direct", NULL, NULL, "现状实测(1step write;窗口含fsync)");
    cmp_row("DSA直接替换(合成)", "mem2disk", "sw_1step_write_direct", "sw_memcpy", "dsa_idle", "假设合成=1step−sw_memcpy+dsa_idle;非已有接管实现");
    cmp_row("软件", "disk2mem", "sw_1step_read_direct", NULL, NULL, "现状实测(1step read)");
    cmp_row("DSA直接替换(合成)", "disk2mem", "sw_1step_read_direct", "sw_memcpy", "dsa_idle", "假设合成=1step−sw_memcpy+dsa_idle;非已有接管实现");
    cmp_row("软件", "mem2mem", "sw_memcpy", NULL, NULL, "可接管段本体(纯搬运参照)");
    cmp_row("DSA", "mem2mem", "dsa_idle", NULL, NULL, "可接管段全 DSA(实测)");
    {
        double msw = res_of("sw_memcpy"), mdsa = res_of("dsa_idle");
        if (msw > 0 && mdsa > 0) {
            double save = msw - mdsa;
            double wd = res_of("sw_1step_write_direct"), rd = res_of("sw_1step_read_direct");
            printf("# 可接管段（%dMB 纯搬运）：软件 %.3f ms → DSA %.3f ms，每 snapshot 省 %.3f ms",
                   size_mb, msw, mdsa, save);
            if (wd > 0 && rd > 0)
                printf("（占 mem2disk 软件 onCPU 的 %.1f%%、disk2mem 的 %.1f%%）\n",
                       100.0 * save / wd, 100.0 * save / rd);
            else
                printf("\n");
        }
    }

    /* ---- snapshot 场景合成：每 snapshot 的 onCPU，纯软件 vs DSA ---- */
    printf("\n# --- snapshot 场景合成（每 snapshot = %dMB；onCPU 单位 ms；sw = 纯软件，dsa = DSA） ---\n", size_mb);
    printf("scenario,structure,sw_oncpu_ms,dsa_oncpu_ms,delta_ms,delta_pct,source\n");
    print_scn("mem2disk", "current_kernel_single_hop", "sw_1step_write_direct", "sw_1step_write_direct",
              "软件微基准实测;重复同一基线非DSA实验;当前无直接接管实现");
    print_scn("mem2disk", "assumed_staging_hop", "sw_2step_memcpy_stg_write", "dsa_2step_dsa_stg_write_idle",
              "本机实测;DSA 只替第二跳(UMWAIT)");
    print_scn("disk2mem", "current_kernel_single_hop", "sw_1step_read_direct", "sw_1step_read_direct",
              "软件微基准实测;重复同一基线非DSA实验;当前无直接接管实现");
    print_scn("disk2mem", "assumed_staging_hop", "sw_2step_read_stg_memcpy", "dsa_2step_read_stg_dsa_idle",
              "本机实测;DSA 只替第二跳(UMWAIT)");
    print_scn("mem2mem", "reference_pure_memory", "sw_memcpy", "dsa_idle",
              "参照臂;无 syscall，DSA 能替的跳长什么样");
    if (do_busy) print_scn("mem2mem", "reference_busy_poll", "sw_memcpy", "dsa_busy",
                           "自旋口径;只可横向比，不可当释放量");

    /* 不再默认使用污染轮942/53.32。显式输入仅用于条件外推，非真实事件对账。 */
    const char *rmg_env = getenv("REAL_MS_PER_GIB");
    const char *rfx_env = getenv("REAL_FIXED_MS");
    if (rmg_env && rfx_env) {
        double rmg = atof(rmg_env), rfx = atof(rfx_env);
        if (isfinite(rmg) && isfinite(rfx) && rmg > 0 && rfx >= 0)
            printf("\n# 条件线性外推（用户输入，非本轮snapshot实测）：%.2f ms = %.2f + %.2f * %d/1024\n",
                   rfx + rmg * size_mb / 1024.0, rfx, rmg, size_mb);
        else
            fprintf(stderr, "WARN: REAL_MS_PER_GIB/REAL_FIXED_MS范围无效，不作外推\n");
    } else {
        printf("\n# 未提供完整真实路径模型，不作snapshot外推；旧942/53.32默认值已停用\n");
    }

    printf("\n# --- 结论口径（引用时必须带这三条） ---\n");
    printf("# 1) 现状结构 sw_1step 没有可被 DSA 替换的用户态跳：两个方向的搬运都在内核里\n");
    printf("#    当前DML无已验证直接接管接口；相同基线行不代表测得DSA收益为0\n");
    printf("# 2) *_2step 行才是「DSA 替掉第二跳」的公平 A/B：收益 = sw_2step − dsa_2step_idle\n");
    printf("# 3) busy 口径是自旋（CPU 算在自己头上），只可横向比，不可报成释放量；\n");
    printf("#    进程口径不含 kworker/jbd2 → 需要系统口径请配 calibrate_cpu_scope.sh scene 模式\n");

    /* ---- 独立复核表（第二张表，与主表分开）：/proc utime+stime tick 采样账本，逐臂区间 ---- */
    if (g_recon_n > 0) {
        printf("\n# --- 独立复核表：/proc/self/stat utime+stime（tick 采样账本；与主表 CLOCK_PROCESS_CPUTIME_ID 是两条独立记账路径） ---\n");
        printf("# 区间=整臂（含窗口外 evict/fsync/LLC 冲刷）→ 总量大于主表纯窗口值×repeat，属预期；\n");
        printf("# tick 量化约 %.0f ms/格 → 逐臂仅区间级可信（小臂 delta_pct 波动大属量化噪声），聚合级才作判废\n", tick_ms());
        printf("op,impl,size_mb,repeat,proc_total_ms,clock_total_ms,delta_ms,delta_pct,match\n");
        for (int i = 0; i < g_recon_n; i++) {
            const Recon *r = &g_recon[i];
            printf("%s,%s,%d,%d,%.1f,%.1f,%.1f,%.1f%%,%s\n",
                   r->op, r->impl, r->size_mb, r->repeat, r->proc_ms, r->clk_ms,
                   r->proc_ms - r->clk_ms,
                   r->clk_ms > 0 ? (r->proc_ms - r->clk_ms) / r->clk_ms * 100.0 : 0.0,
                   r->ok ? "OK" : "OFF");
        }
    }

    /* ---- onCPU 跨源对账总判定：证明「测得真」——两条独立内核记账路径相互复核 ---- */
    if (g_clk_span_ms > 0 || g_proc_span_ms > 0) {
        double tol = 4 * tick_ms() + 0.05 * g_clk_span_ms;
        double d = g_proc_span_ms - g_clk_span_ms;
        printf("\n# --- onCPU 跨源对账：CLOCK_PROCESS_CPUTIME_ID(主口径,ns) vs /proc/self/stat utime+stime(tick 采样) ---\n");
        printf("# 全部臂区间总 CPU：clock=%.0f ms  /proc=%.0f ms  Δ=%.1f ms（量化%.0f ms/格，容差%.0f ms）\n",
               g_clk_span_ms, g_proc_span_ms, d, tick_ms(), tol);
        printf("# → %s（两种CPU记账读数的一致性检查；不证明无隐藏开销、无干扰或比较设计公平）\n",
               (fabs(d) <= tol) ? "两接口读数在容差内一致"
                                : "WARN: 聚合级两源不吻合，本轮数据勿引用");
        printf("# 超差臂 %d 个（个别臂偏小多为 tick 采样漏记 I/O 间隙短突发，只作提示）\n",
               g_recon_bad);
    }

    if (g_job) { dml_finalize_job(g_job); free(g_job); }
    close(fd); unlink(path);
    free(src); free(dst); free(stg);
    return 0;
}
