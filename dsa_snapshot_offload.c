// dsa_snapshot_offload.c — 阶段 4.2 补充：DSA 卸载 memory copy 阶段的 CPU 下降比例
//
// 模拟 AgentENV snapshot 的 memory copy 形态：
//   源   = 散落 dirty 页（独立 mmap 4KB，模拟 guest 物理散落的 dirty 页）
//   目标 = 连续 buffer（模拟 process_vm_readv 读出来、写给 commit 文件的 buffer）
//
// 对比两条搬运路径的「每 GB CPU 时间」：
//   1. CPU 逐页 memcpy（rep movsb，模拟 process_vm_readv 的内核逐页拷贝）
//   2. DSA batch 异步（N 页打包 batch 描述符 + dml_submit_job 提交后让出 CPU）
//
// 核心指标：每 GB CPU 时间（ms/GB）与 CPU 下降比例，直接量化 DSA 卸载收益。
// 注意：这测的是「显性 copy 指令」的 CPU 下降；搬运的隐性成本（cache 污染、
// 内存带宽争用对 ext4 等其他逻辑的拖累）不在本 benchmark 内，需端到端接入再测。

#include <dml/dml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define BLOCK_SIZE   4096
#define TOTAL_BLOCKS 32768   // 128MB（> L3，冷页口径）
#define BATCH_N      48      // batch 大小（> 阈值 N≈48，复用阶段 4.1 结论）
#define REPEAT       10      // 每个配置重复次数，取平均抗偶然性

static double cpu_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// 冲刷 CPU cache，让测量从「冷 cache」开始（冷页口径）。
static void flush_cache(void) {
    static uint8_t *dummy = NULL;
    static const size_t kCache = 256 * 1024 * 1024;  // 256MB > L3
    volatile uint64_t sink = 0;
    if (!dummy) dummy = malloc(kCache);
    for (size_t i = 0; i < kCache; i += 64) dummy[i] = (uint8_t)(i & 0xFF);
    for (size_t i = 0; i < kCache; i += 64) sink += dummy[i];
    (void)sink;
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

// CPU 逐页 memcpy（散落源 -> 连续目标）。返回每 GB CPU 时间（秒），wall 时间经 *wall_out 带出。
static double cpu_scattered_copy(uint8_t **src, uint8_t *dst, int blocks, double *wall_out) {
    flush_cache();
    double t0 = cpu_time_sec(), w0 = wall_time_sec();
    for (int i = 0; i < blocks; i++)
        memcpy(dst + (size_t)i * BLOCK_SIZE, src[i], BLOCK_SIZE);
    double t1 = cpu_time_sec(), w1 = wall_time_sec();
    *wall_out = w1 - w0;
    return (t1 - t0) / ((double)blocks * BLOCK_SIZE / 1e9);
}

// DSA batch 异步（散落源 -> 连续目标）。返回每 GB CPU 时间（秒），wall 时间经 *wall_out 带出。
static double dsa_batch_async(uint8_t **src, uint8_t *dst, int blocks, int n, double *wall_out) {
    uint32_t job_size = 0;
    dml_get_job_size(DML_PATH_HW, &job_size);
    dml_job_t *job = malloc(job_size);
    dml_init_job(DML_PATH_HW, job);
    job->operation = DML_OP_BATCH;   // 必须先设 operation，dml_get_batch_size 依赖它

    uint32_t batch_bytes = 0;
    dml_status_t st = dml_get_batch_size(job, (uint32_t)n, &batch_bytes);
    if (st != DML_STATUS_OK) {
        fprintf(stderr, "dml_get_batch_size(n=%d) failed: %u\n", n, (unsigned)st);
        dml_finalize_job(job); free(job); return -1.0;
    }
    uint8_t *batch = malloc(batch_bytes);
    job->destination_first_ptr = batch;
    job->destination_length = batch_bytes;

    int batches = blocks / n;
    flush_cache();
    double t0 = cpu_time_sec(), w0 = wall_time_sec();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < n; i++) {
            int idx = b * n + i;
            dml_batch_set_mem_move_by_index(job, (uint32_t)i,
                src[idx], dst + (size_t)idx * BLOCK_SIZE, BLOCK_SIZE, 0);
        }
        dml_submit_job(job);                        // 异步提交，立即返回
        // usleep(1000);                            // 验证：注释掉 usleep，改 busy-poll 等待（等价阶段3 的 dml_execute_job）
        dml_wait_job(job, DML_WAIT_MODE_BUSY_POLL); // busy-poll 等 DSA 完成（CPU 100% spin）
    }
    double t1 = cpu_time_sec(), w1 = wall_time_sec();
    *wall_out = w1 - w0;

    dml_finalize_job(job);
    free(batch);
    free(job);

    double moved = (double)batches * (double)n * BLOCK_SIZE;
    return (t1 - t0) / (moved / 1e9);
}

int main(void) {
    uint8_t **src = alloc_scattered(0x5A);
    uint8_t *dst = malloc((size_t)TOTAL_BLOCKS * BLOCK_SIZE);
    if (!dst) { perror("malloc"); return 1; }
    memset(dst, 0x00, (size_t)TOTAL_BLOCKS * BLOCK_SIZE);

    int blocks = (TOTAL_BLOCKS / BATCH_N) * BATCH_N;  // 对齐 batch，两路径搬同量
    double moved_mb = (double)blocks * BLOCK_SIZE / 1e6;

    double cpu_v[REPEAT], cpu_wall[REPEAT];
    double dsa_v[REPEAT], dsa_wall[REPEAT];
    for (int r = 0; r < REPEAT; r++) cpu_v[r] = cpu_scattered_copy(src, dst, blocks, &cpu_wall[r]);
    for (int r = 0; r < REPEAT; r++) dsa_v[r] = dsa_batch_async(src, dst, blocks, BATCH_N, &dsa_wall[r]);

    double cpu_avg = 0, cpu_w = 0, dsa_avg = 0, dsa_w = 0;
    for (int r = 0; r < REPEAT; r++) { cpu_avg += cpu_v[r]; cpu_w += cpu_wall[r]; dsa_avg += dsa_v[r]; dsa_w += dsa_wall[r]; }
    cpu_avg /= REPEAT; cpu_w /= REPEAT; dsa_avg /= REPEAT; dsa_w /= REPEAT;

    double moved_gb = moved_mb / 1000.0;
    printf("搬运量: %d x 4KB = %.2f MB（散落源 -> 连续目标，冷页口径，batch N=%d）\n\n",
           blocks, moved_mb, BATCH_N);
    printf("%-22s | %10s | %9s | %8s\n", "路径", "每GB CPU", "wall(s)", "CPU占用");
    printf("%-22s | %8.1fms | %8.4f | %7.2f%%\n",
           "CPU 逐页 memcpy", cpu_avg * 1000, cpu_w, cpu_avg * moved_gb / cpu_w * 100.0);
    printf("%-22s | %8.1fms | %8.4f | %7.2f%%\n",
           "DSA batch 异步", dsa_avg * 1000, dsa_w, dsa_avg * moved_gb / dsa_w * 100.0);

    double drop = (1.0 - dsa_avg / cpu_avg) * 100.0;
    printf("\nDSA 卸载 memory copy 的 CPU 下降比例 = %.1f%%\n", drop);
    printf("（注意：此为显性 copy 指令的下降；隐性 cache 污染/带宽收益需端到端再测）\n");

    for (int i = 0; i < TOTAL_BLOCKS; i++) munmap(src[i], BLOCK_SIZE);
    free(src);
    free(dst);
    return 0;
}
