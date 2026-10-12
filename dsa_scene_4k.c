// dsa_scene_4k.c — 阶段 4.1 场景建模：散落页 batch + 页温度
//
// 回答阶段 3 遗留的两个「可行性前提」，供 snapshot 场景的 Data Stream Offload 判断：
//   1. 散落页 batch：物理不连续的 4KB 页打包进 DSA batch，是否仍拿 5~11×
//      （阶段 3 的 dsa_batch_4k.c 用连续内存；snapshot 真实页物理散落）
//   2. 页温度：CPU memcpy 基线在「热页(cache 常驻)」vs「冷页(超出 cache)」下的差异，
//      决定 DSA 5~11× 反超的适用条件（DSA 走 DMA 读内存，对 cache 冷热不敏感）
//
// 对比：连续 4KB 块 batch vs 散落 4KB 页(独立 mmap) batch；冷/热 CPU 基线。

#include <dml/dml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#define BLOCK_SIZE   4096
#define TOTAL_BLOCKS 4096  // 16MB
#define REPEAT       3

static double wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// 冲刷 CPU cache，让测量从「冷 cache」开始。
static void flush_cache(void) {
    static uint8_t *dummy = NULL;
    static const size_t kCache = 256 * 1024 * 1024;  // 256MB > L3
    volatile uint64_t sink = 0;
    if (!dummy) dummy = malloc(kCache);
    for (size_t i = 0; i < kCache; i += 64) dummy[i] = (uint8_t)(i & 0xFF);
    for (size_t i = 0; i < kCache; i += 64) sink += dummy[i];
    (void)sink;
}

// CPU 冷页基线：16MB 连续 4KB 块逐个 memcpy（flush 后，超出 cache）。
static double bench_cpu_cold(uint8_t *src, uint8_t *dst) {
    flush_cache();
    double w0 = wall_time_sec();
    for (int i = 0; i < TOTAL_BLOCKS; i++)
        memcpy(dst + i * BLOCK_SIZE, src + i * BLOCK_SIZE, BLOCK_SIZE);
    double w1 = wall_time_sec();
    return (double)TOTAL_BLOCKS * BLOCK_SIZE / 1e9 / (w1 - w0);
}

// CPU 热页基线：反复搬 64KB 小块（常驻 L2/L3，cache 带宽）。
static double bench_cpu_hot(void) {
    const size_t sz = 64 * 1024;
    uint8_t *s = malloc(sz), *d = malloc(sz);
    if (!s || !d) { perror("malloc"); exit(1); }
    memset(s, 0x5A, sz);
    flush_cache();
    long iters = 200000;
    double w0 = wall_time_sec();
    for (long i = 0; i < iters; i++) memcpy(d, s, sz);
    double w1 = wall_time_sec();
    double gbs = (double)iters * (double)sz / 1e9 / (w1 - w0);
    free(s); free(d);
    return gbs;
}

// DSA batch：scattered=0 用连续基址 + idx 偏移；scattered=1 用独立散落的页指针数组。
static double bench_dsa_batch(uint8_t *src_base, uint8_t *dst_base,
                              uint8_t **src_ptrs, uint8_t **dst_ptrs,
                              int scattered, int n) {
    uint32_t job_size = 0;
    dml_get_job_size(DML_PATH_HW, &job_size);
    dml_job_t *job = malloc(job_size);
    dml_init_job(DML_PATH_HW, job);
    job->operation = DML_OP_BATCH;

    uint32_t batch_bytes = 0;
    dml_status_t st = dml_get_batch_size(job, (uint32_t)n, &batch_bytes);
    if (st != DML_STATUS_OK) {
        fprintf(stderr, "dml_get_batch_size(n=%d) failed: %u\n", n, (unsigned)st);
        dml_finalize_job(job); free(job); return 0.0;
    }
    uint8_t *batch = malloc(batch_bytes);
    job->destination_first_ptr = batch;
    job->destination_length = batch_bytes;

    int batches = TOTAL_BLOCKS / n;
    flush_cache();
    double w0 = wall_time_sec();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < n; i++) {
            int idx = b * n + i;
            uint8_t *s = scattered ? src_ptrs[idx] : src_base + idx * BLOCK_SIZE;
            uint8_t *d = scattered ? dst_ptrs[idx] : dst_base + idx * BLOCK_SIZE;
            dml_batch_set_mem_move_by_index(job, (uint32_t)i, s, d, BLOCK_SIZE, 0);
        }
        dml_execute_job(job, DML_WAIT_MODE_BUSY_POLL);
    }
    double w1 = wall_time_sec();
    double moved = (double)((size_t)batches * (size_t)n) * BLOCK_SIZE;
    double gbs = moved / 1e9 / (w1 - w0);

    dml_finalize_job(job);
    free(batch);
    free(job);
    return gbs;
}

// 分配 TOTAL_BLOCKS 个独立匿名映射的 4KB 页（物理散落）。
static uint8_t **alloc_scattered(uint8_t fill) {
    uint8_t **ptrs = malloc(TOTAL_BLOCKS * sizeof(uint8_t *));
    if (!ptrs) { perror("malloc"); exit(1); }
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        ptrs[i] = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptrs[i] == MAP_FAILED) { perror("mmap"); exit(1); }
        memset(ptrs[i], fill, BLOCK_SIZE);
    }
    return ptrs;
}

static void free_scattered(uint8_t **ptrs) {
    for (int i = 0; i < TOTAL_BLOCKS; i++) munmap(ptrs[i], BLOCK_SIZE);
    free(ptrs);
}

int main(void) {
    // 连续 16MB
    uint8_t *src_c = malloc((size_t)TOTAL_BLOCKS * BLOCK_SIZE);
    uint8_t *dst_c = malloc((size_t)TOTAL_BLOCKS * BLOCK_SIZE);
    if (!src_c || !dst_c) { perror("malloc"); return 1; }
    memset(src_c, 0x5A, (size_t)TOTAL_BLOCKS * BLOCK_SIZE);
    memset(dst_c, 0x00, (size_t)TOTAL_BLOCKS * BLOCK_SIZE);

    // 散落：独立 mmap 的 4KB 页
    uint8_t **src_s = alloc_scattered(0x5A);
    uint8_t **dst_s = alloc_scattered(0x00);

    // ---- 页温度：CPU 冷/热基线 ----
    double cold = 0.0, hot = 0.0;
    for (int r = 0; r < REPEAT; r++) cold += bench_cpu_cold(src_c, dst_c);
    for (int r = 0; r < REPEAT; r++) hot += bench_cpu_hot();
    cold /= REPEAT; hot /= REPEAT;
    printf("[CPU 冷页基线 (16MB flush)] %.2f GB/s\n", cold);
    printf("[CPU 热页基线 (64KB cache)]  %.2f GB/s\n", hot);
    printf("热/冷比 %.2fx  => 页越冷，CPU 掉速越多，DSA 反超空间越大\n\n", hot / cold);

    // ---- 散落页 batch vs 连续 batch（N 扫描，阈值附近 + 典型值）----
    int ns[] = {32, 48, 64, 128, 256};
    printf("%-8s | %-16s | %-16s\n", "N", "连续 batch GB/s", "散落 batch GB/s");
    for (size_t k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
        int n = ns[k];
        double sum_c = 0.0, sum_s = 0.0;
        for (int r = 0; r < REPEAT; r++)
            sum_c += bench_dsa_batch(src_c, dst_c, NULL, NULL, 0, n);
        for (int r = 0; r < REPEAT; r++)
            sum_s += bench_dsa_batch(NULL, NULL, src_s, dst_s, 1, n);
        double avg_c = sum_c / REPEAT, avg_s = sum_s / REPEAT;
        printf("%-8d | %-16.2f | %-16.2f  (散落/连续 %.2fx)\n",
               n, avg_c, avg_s, avg_s / avg_c);
    }

    free(src_c); free(dst_c);
    free_scattered(src_s); free_scattered(dst_s);
    return 0;
}