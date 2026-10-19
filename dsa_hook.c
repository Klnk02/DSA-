// dsa_hook.c — LD_PRELOAD 拦截器：在真实进程（uvm-ublk-daemon / firecracker）内部，
//              把 read/write 的「用户态拷贝段」改走 DSA（DML batch mem_move），
//              或用纯 memcpy 走同一条路径作对照。用于「真穿刺」A/B。
//
// 为什么要这样设计（务必先读，否则会误读结论）：
//   DSA（DML mem_move）只能卸载「用户态 mem2mem 拷贝」。
//   read()/write() 自带的内核拷贝（copy_to_user / copy_from_user / dma）无法被用户态 DSA 卸载。
//   因此本拦截器无法让 snapshot 的磁盘侧搬运变快；它做的是：
//     ① 在真实守护进程内部，用同一条代码路径对比 DSA vs memcpy（受控 A/B）
//     ② 在真实 snapshot 现场测出守护进程的真实 onCPU（配合外部 /proc 采样）
//   若守护进程真实路径里本就没有用户态 memcpy，DSA 收益 = 0 —— 这本身就是要交付的结论。
//
// 三臂口径：
//   R0  完全不挂钩        = 真实软件基线（零用户态拷贝）
//   R1  LD_PRELOAD + MODE=memcpy  = 挂钩开销对照（多一次用户态 memcpy）
//   R2  LD_PRELOAD + MODE=dsa     = 把那次 memcpy 换成 DML
//   结论：R2 vs R1 = DSA 能否打赢 memcpy；R1 vs R0 = 挂钩自身引入的成本。
//
// 编译（服务器，只用头文件取类型/偏移，DML 函数运行时 dlopen 解析，故不链接 libdml）：
//   gcc -O2 -Wall -fPIC -shared -I$HOME/.local/include dsa_hook.c -o dsa_hook.so -ldl -pthread
//
// 运行：
//   LD_PRELOAD=$PWD/dsa_hook.so DSA_HOOK_MODE=dsa    ./uvm-ublk-daemon ...
//   LD_PRELOAD=$PWD/dsa_hook.so DSA_HOOK_MODE=memcpy ./uvm-ublk-daemon ...
//
// 环境变量：
//   DSA_HOOK_MODE = memcpy | dsa | off   （off = 直接旁路，仅计数；默认 off，避免误伤）
//   DSA_LIB       = libdml.so 路径（默认 $HOME/.local/lib/libdml.so）
//   DSA_MIN_KB    = 触发分段的长度下限（默认 64）
//   DSA_CHUNK_MB  = 单次分段窗口（默认 8）
//   DSA_HOOK_LOG  = 统计输出文件（默认 /tmp/dsa_hook.stats）
//   DSA_FD_FILTER = 1 只对普通文件/块设备生效（默认 1，避免误伤 socket/pipe/stdio）

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/stat.h>

#include <dml/dml.h>            /* 仅用类型/枚举/结构体偏移；函数运行时 dlsym 解析 */

#define PG        4096u
#define BATCH_N   32u           /* shared wq max_batch_size 上限（2026-10-16 实测 48 报错 29） */

/* ---------------- 真实函数（RTLD_NEXT） ---------------- */
static ssize_t (*real_read)  (int, void *, size_t);
static ssize_t (*real_write) (int, const void *, size_t);
static ssize_t (*real_pread) (int, void *, size_t, off_t);
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static void   *(*real_memcpy) (void *, const void *, size_t);
static void   *(*real_memmove)(void *, const void *, size_t);

/* ---------------- DML 运行时函数指针 ---------------- */
typedef dml_status_t (*fn_get_job_size)  (dml_path_t, uint32_t *);
typedef dml_status_t (*fn_init_job)      (dml_path_t, dml_job_t *);
typedef dml_status_t (*fn_get_batch_size)(const dml_job_t *, uint32_t, uint32_t *);
typedef dml_status_t (*fn_set_mem_move)  (dml_job_t *, uint32_t, void *, void *, uint32_t, uint32_t);
typedef dml_status_t (*fn_submit_job)    (dml_job_t *);
typedef dml_status_t (*fn_wait_job)      (dml_job_t *, dml_wait_mode_t);
typedef dml_status_t (*fn_free_job)      (dml_job_t *);

static fn_get_job_size   f_get_job_size;
static fn_init_job       f_init_job;
static fn_get_batch_size f_get_batch_size;
static fn_set_mem_move   f_set_mem_move;
static fn_submit_job     f_submit_job;
static fn_wait_job       f_wait_job;
static fn_free_job       f_free_job;

/* ---------------- 配置 ---------------- */
enum { MODE_OFF = 0, MODE_MEMCPY = 1, MODE_DSA = 2 };
static int      g_mode       = MODE_OFF;
static int      g_dsa_ok     = 0;
static int      g_fd_filter  = 1;
static size_t   g_min_bytes  = 64u << 10;
static size_t   g_chunk      = 8u << 20;
static char     g_log[256]   = "/tmp/dsa_hook.stats";
static int      g_logfd      = -1;
static int      g_init_done  = 0;
static int      g_init_running = 0;

/* ---------------- 统计 ---------------- */
static unsigned long long g_calls[6];      /* read, write, pread, pwrite, memcpy, memmove */
static unsigned long long g_bytes[6];
static unsigned long long g_steer_bytes;   /* 真正走过分段（memcpy 或 DSA）的字节 */
static unsigned long long g_dsa_bytes;     /* 其中由 DML 搬运的字节 */
static unsigned long long g_dsa_calls;
static unsigned long long g_pass_bytes;    /* 直通字节 */
static double             g_hook_cpu;      /* 挂钩内部（steer）消耗的进程 CPU 秒 */

static double cpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 统计落盘：只用 real_write，绝不走 stdio（否则递归进本 hook） */
static void dump_stats(void)
{
    if (g_logfd < 0) g_logfd = open(g_log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_logfd < 0) return;
    char b[1024];
    int m = snprintf(b, sizeof b,
        "mode=%s dsa_ok=%d min_kb=%zu chunk_mb=%zu\n"
        "hook_cpu_s=%.4f\n"
        "calls  read=%llu write=%llu pread=%llu pwrite=%llu memcpy=%llu memmove=%llu\n"
        "bytes  read=%llu write=%llu pread=%llu pwrite=%llu memcpy=%llu memmove=%llu\n"
        "pass_bytes=%llu steer_bytes=%llu dsa_bytes=%llu dsa_calls=%llu\n",
        g_mode == MODE_DSA ? "dsa" : (g_mode == MODE_MEMCPY ? "memcpy" : "off"),
        g_dsa_ok, g_min_bytes >> 10, g_chunk >> 20, g_hook_cpu,
        g_calls[0], g_calls[1], g_calls[2], g_calls[3], g_calls[4], g_calls[5],
        g_bytes[0], g_bytes[1], g_bytes[2], g_bytes[3], g_bytes[4], g_bytes[5],
        g_pass_bytes, g_steer_bytes, g_dsa_bytes, g_dsa_calls);
    if (m > 0 && real_write) real_write(g_logfd, b, (size_t)m);
}

static void on_signal(int sig)
{
    dump_stats();
    _exit(128 + sig);
}

/* ---------------- 线程局部：staging 缓冲 + DML job ---------------- */
typedef struct {
    uint8_t   *base;        /* 页对齐基址 */
    size_t     base_sz;
    dml_job_t *job;
    int        job_bad;
    int        busy;        /* 递归保护：steer 内部调用 real_memcpy 时不再进钩子 */
} tls_t;

static pthread_key_t  g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void tls_free(void *p)
{
    tls_t *t = (tls_t *)p;
    if (!t) return;
    if (t->job) { if (f_free_job) f_free_job(t->job); free(t->job); }
    free(t->base);
    free(t);
}

static tls_t *tls_get(void)
{
    tls_t *t = (tls_t *)pthread_getspecific(g_key);
    if (t) return t;
    t = (tls_t *)calloc(1, sizeof *t);
    if (!t) return NULL;
    pthread_setspecific(g_key, t);
    return t;
}

/* staging 缓冲：保证 (stage - src) 是 4096 的整数倍 → src/off 与 stage/off 同时页对齐 */
static uint8_t *stage_get(tls_t *t, size_t need, const void *src)
{
    size_t want = need + PG;
    if (want > g_chunk + PG) want = g_chunk + PG;
    if (!t->base || t->base_sz < want) {
        uint8_t *nb = NULL;
        if (posix_memalign((void **)&nb, PG, want) != 0) return NULL;
        free(t->base);
        t->base = nb;
        t->base_sz = want;
    }
    size_t mod = (size_t)((uintptr_t)src & (PG - 1));
    return t->base + mod;
}

static void tls_dsa_init(tls_t *t)
{
    if (t->job || t->job_bad) return;
    if (!f_get_job_size) { t->job_bad = 1; return; }
    uint32_t sz = 0;
    if (f_get_job_size(DML_PATH_HW, &sz) != DML_STATUS_OK || sz == 0) { t->job_bad = 1; return; }
    dml_job_t *j = (dml_job_t *)malloc(sz);
    if (!j) { t->job_bad = 1; return; }
    if (f_init_job(DML_PATH_HW, j) != DML_STATUS_OK) { free(j); t->job_bad = 1; return; }
    j->operation = DML_OP_BATCH;
    uint32_t bb = 0;
    if (f_get_batch_size(j, BATCH_N, &bb) != DML_STATUS_OK || bb == 0) { free(j); t->job_bad = 1; return; }
    uint8_t *batch = (uint8_t *)malloc(bb);
    if (!batch) { free(j); t->job_bad = 1; return; }
    memset(batch, 0, bb);
    j->destination_first_ptr = batch;
    j->destination_length    = bb;
    t->job = j;
}

/* DML batch 搬 pages 页（调用前 src/dst 均已 4096 对齐、n 为 4096 整数倍） */
static void dsa_move(uint8_t *dst, uint8_t *src, size_t n, tls_t *t)
{
    tls_dsa_init(t);
    if (!t->job) { real_memcpy(dst, src, n); return; }
    uint64_t pages = n / PG, batches = pages / BATCH_N;
    for (uint64_t b = 0; b < batches; b++) {
        for (uint32_t i = 0; i < BATCH_N; i++) {
            uint64_t k = b * BATCH_N + i;
            f_set_mem_move(t->job, i, src + k * PG, dst + k * PG, PG, 0);
        }
        f_submit_job(t->job);
        f_wait_job(t->job, DML_WAIT_MODE_UMWAIT);   /* 低功耗等待：CPU 真正空闲 */
    }
    for (uint64_t k = batches * BATCH_N; k < pages; k++)
        real_memcpy(dst + k * PG, src + k * PG, PG);
    g_dsa_bytes += n;
    g_dsa_calls++;
}

/* 分段搬运：非页对齐的头/尾用 memcpy，中间页对齐段按 MODE 走 memcpy 或 DML。
   stage 已保证与 src 同模 4096 → off 对两边同时成立。 */
static void steer(void *dst, const void *src, size_t n, tls_t *t)
{
    size_t off = ((PG - ((uintptr_t)src & (PG - 1))) & (PG - 1));
    if (off > n) off = n;
    size_t mid = (n - off) & ~(size_t)(PG - 1);
    if (off) real_memcpy(dst, src, off);
    if (mid) {
        uint8_t *s = (uint8_t *)src + off, *d = (uint8_t *)dst + off;
        if (g_mode == MODE_DSA && g_dsa_ok) dsa_move(d, s, mid, t);
        else real_memcpy(d, s, mid);
    }
    size_t tail = n - off - mid;
    if (tail) real_memcpy((uint8_t *)dst + off + mid, (const uint8_t *)src + off + mid, tail);
    g_steer_bytes += n;
}

static void count(int kind, size_t bytes)
{
    __sync_fetch_and_add(&g_calls[kind], 1);
    __sync_fetch_and_add(&g_bytes[kind], (unsigned long long)bytes);
}

/* 只有普通文件/块设备才值得拦（socket/pipe/stdio 直通，避免破坏 IPC 与日志） */
static int fd_ok(int fd)
{
    if (!g_fd_filter) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    return S_ISREG(st.st_mode) || S_ISBLK(st.st_mode);
}

/* ---------------- 初始化 ---------------- */
/* dlsym 失败时的兜底：绝不回落到本文件定义的 memcpy（会自递归），用朴素逐字节拷贝 */
static void *naive_copy(void *d, const void *s, size_t n)
{
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}

static void do_init(void)
{
    const char *e;
    if (g_init_running || g_init_done) return;   /* dlsym 内部可能再进钩子 → 防重入 */
    g_init_running = 1;
    if ((e = getenv("DSA_HOOK_MODE"))) {
        if (!strcmp(e, "dsa"))         g_mode = MODE_DSA;
        else if (!strcmp(e, "memcpy")) g_mode = MODE_MEMCPY;
        else                           g_mode = MODE_OFF;
    }
    if ((e = getenv("DSA_MIN_KB")) && atoi(e) > 0)   g_min_bytes = (size_t)atoi(e) << 10;
    if ((e = getenv("DSA_CHUNK_MB")) && atoi(e) > 0) g_chunk     = (size_t)atoi(e) << 20;
    if ((e = getenv("DSA_HOOK_LOG")))                snprintf(g_log, sizeof g_log, "%s", e);
    if ((e = getenv("DSA_FD_FILTER")))               g_fd_filter = atoi(e);

    real_read   = dlsym(RTLD_NEXT, "read");
    real_write  = dlsym(RTLD_NEXT, "write");
    real_pread  = dlsym(RTLD_NEXT, "pread");
    real_pwrite = dlsym(RTLD_NEXT, "pwrite");
    real_memcpy = dlsym(RTLD_NEXT, "memcpy");
    real_memmove= dlsym(RTLD_NEXT, "memmove");
    if (!real_memcpy)  real_memcpy  = naive_copy;   /* 绝不回落到本文件定义的 memcpy（会自递归） */
    if (!real_memmove) real_memmove = naive_copy;

    if (g_mode == MODE_DSA) {
        const char *lib = getenv("DSA_LIB");
        char def[512];
        if (!lib) {
            const char *h = getenv("HOME");
            snprintf(def, sizeof def, "%s/.local/lib/libdml.so", h ? h : "");
            lib = def;
        }
        void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
        if (!h) h = dlopen("libdml.so", RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            f_get_job_size   = (fn_get_job_size)  dlsym(h, "dml_get_job_size");
            f_init_job       = (fn_init_job)      dlsym(h, "dml_init_job");
            f_get_batch_size = (fn_get_batch_size)dlsym(h, "dml_get_batch_size");
            f_set_mem_move   = (fn_set_mem_move)  dlsym(h, "dml_batch_set_mem_move_by_index");
            f_submit_job     = (fn_submit_job)    dlsym(h, "dml_submit_job");
            f_wait_job       = (fn_wait_job)      dlsym(h, "dml_wait_job");
            f_free_job       = (fn_free_job)      dlsym(h, "dml_free_job");
            if (f_get_job_size && f_init_job && f_get_batch_size && f_set_mem_move &&
                f_submit_job && f_wait_job) g_dsa_ok = 1;
        }
    }
    g_init_done = 1;
    g_init_running = 0;
}

static void key_once(void) { pthread_key_create(&g_key, tls_free); }

__attribute__((constructor))
static void hook_ctor(void)
{
    /* 构造函数里只装最必要的：dlsym + 配置。DML 延后到首次使用时再解析，避免拖慢启动 */
    do_init();
    pthread_once(&g_once, key_once);
    atexit(dump_stats);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
}

/* ---------------- 被拦截的系统调用 ---------------- */
ssize_t read(int fd, void *buf, size_t n)
{
    if (!g_init_done) do_init();
    if (!real_read) { errno = ENOSYS; return -1; }
    if (g_mode == MODE_OFF || !buf || n < g_min_bytes || !fd_ok(fd)) {
        ssize_t r = real_read(fd, buf, n);
        count(0, r > 0 ? (size_t)r : 0);
        if (r > 0) g_pass_bytes += (unsigned long long)r;
        return r;
    }
    tls_t *t = tls_get();
    if (!t) return real_read(fd, buf, n);
    size_t done = 0;
    while (done < n) {
        size_t c = n - done; if (c > g_chunk) c = g_chunk;
        uint8_t *st = stage_get(t, c, (uint8_t *)buf + done);
        if (!st) break;
        ssize_t r = real_read(fd, st, c);
        if (r <= 0) { if (done) break; return r; }
        double c0 = cpu_now();
        steer((uint8_t *)buf + done, st, (size_t)r, t);
        g_hook_cpu += cpu_now() - c0;
        done += (size_t)r;
        if ((size_t)r < c) break;
    }
    count(0, done);
    return (ssize_t)done;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    if (!g_init_done) do_init();
    if (!real_write) { errno = ENOSYS; return -1; }
    if (g_mode == MODE_OFF || !buf || n < g_min_bytes || !fd_ok(fd)) {
        ssize_t r = real_write(fd, buf, n);
        count(1, r > 0 ? (size_t)r : 0);
        if (r > 0) g_pass_bytes += (unsigned long long)r;
        return r;
    }
    tls_t *t = tls_get();
    if (!t) return real_write(fd, buf, n);
    size_t done = 0;
    while (done < n) {
        size_t c = n - done; if (c > g_chunk) c = g_chunk;
        uint8_t *st = stage_get(t, c, (const uint8_t *)buf + done);
        if (!st) break;
        double c0 = cpu_now();
        steer(st, (const uint8_t *)buf + done, c, t);
        g_hook_cpu += cpu_now() - c0;
        ssize_t r = real_write(fd, st, c);
        if (r <= 0) { if (done) break; return r; }
        done += (size_t)r;
        if ((size_t)r < c) break;
    }
    count(1, done);
    return (ssize_t)done;
}

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    if (!g_init_done) do_init();
    if (!real_pread) { errno = ENOSYS; return -1; }
    if (g_mode == MODE_OFF || !buf || n < g_min_bytes || !fd_ok(fd)) {
        ssize_t r = real_pread(fd, buf, n, off);
        count(2, r > 0 ? (size_t)r : 0);
        if (r > 0) g_pass_bytes += (unsigned long long)r;
        return r;
    }
    tls_t *t = tls_get();
    if (!t) return real_pread(fd, buf, n, off);
    size_t done = 0;
    while (done < n) {
        size_t c = n - done; if (c > g_chunk) c = g_chunk;
        uint8_t *st = stage_get(t, c, (uint8_t *)buf + done);
        if (!st) break;
        ssize_t r = real_pread(fd, st, c, off + (off_t)done);
        if (r <= 0) { if (done) break; return r; }
        double c0 = cpu_now();
        steer((uint8_t *)buf + done, st, (size_t)r, t);
        g_hook_cpu += cpu_now() - c0;
        done += (size_t)r;
        if ((size_t)r < c) break;
    }
    count(2, done);
    return (ssize_t)done;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (!g_init_done) do_init();
    if (!real_pwrite) { errno = ENOSYS; return -1; }
    if (g_mode == MODE_OFF || !buf || n < g_min_bytes || !fd_ok(fd)) {
        ssize_t r = real_pwrite(fd, buf, n, off);
        count(3, r > 0 ? (size_t)r : 0);
        if (r > 0) g_pass_bytes += (unsigned long long)r;
        return r;
    }
    tls_t *t = tls_get();
    if (!t) return real_pwrite(fd, buf, n, off);
    size_t done = 0;
    while (done < n) {
        size_t c = n - done; if (c > g_chunk) c = g_chunk;
        uint8_t *st = stage_get(t, c, (const uint8_t *)buf + done);
        if (!st) break;
        double c0 = cpu_now();
        steer(st, (const uint8_t *)buf + done, c, t);
        g_hook_cpu += cpu_now() - c0;
        ssize_t r = real_pwrite(fd, st, c, off + (off_t)done);
        if (r <= 0) { if (done) break; return r; }
        done += (size_t)r;
        if ((size_t)r < c) break;
    }
    count(3, done);
    return (ssize_t)done;
}

/* ---------------- 用户态 mem2mem：唯一真正能被 DSA 卸载的段 ---------------- */
void *memcpy(void *dst, const void *src, size_t n)
{
    if (!g_init_done) do_init();
    void *(*rc)(void *, const void *, size_t) = real_memcpy ? real_memcpy : naive_copy;
    if (g_mode == MODE_OFF || n < g_min_bytes) { count(4, n); return rc(dst, src, n); }
    tls_t *t = tls_get();
    if (!t || t->busy) { count(4, n); return rc(dst, src, n); }
    t->busy = 1;
    double c0 = cpu_now();
    steer(dst, src, n, t);
    g_hook_cpu += cpu_now() - c0;
    t->busy = 0;
    count(4, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    if (!g_init_done) do_init();
    void *(*rc)(void *, const void *, size_t) = real_memmove ? real_memmove : naive_copy;
    /* 区间重叠时 DSA 会破坏语义 → 一律交回真实 memmove */
    uintptr_t a = (uintptr_t)dst, b = (uintptr_t)src;
    int overlap = (a < b + n) && (b < a + n);
    if (g_mode == MODE_OFF || n < g_min_bytes || overlap) { count(5, n); return rc(dst, src, n); }
    tls_t *t = tls_get();
    if (!t || t->busy) { count(5, n); return rc(dst, src, n); }
    t->busy = 1;
    double c0 = cpu_now();
    steer(dst, src, n, t);
    g_hook_cpu += cpu_now() - c0;
    t->busy = 0;
    count(5, n);
    return dst;
}
