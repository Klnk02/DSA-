// dsa_batch_4k.c — 4KB 页操作的 DSA Batch 打包测试（阶段 3：场景建模）
//
// 背景：单发 4KB memcpy DSA 慢 CPU 78×（固定提交开销 ~4.8us）。Batch 把 N 个
// 4KB 操作打包成 1 个描述符、只付 1 次提交开销，验证能否摊薄开销挽回劣势。
//
// 对比：CPU 逐个 4KB memcpy vs DSA Batch（N 个 4KB/批次），扫 N = 4~256。
// 指标：吞吐 GB/s。

#include <dml/dml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLOCK_SIZE   4096
#define TOTAL_BLOCKS 4096  // 16MB
#define REPEAT       3      // 每个配置重复次数，取平均抗噪，验证跳变是否稳定

static double wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// 冲刷 CPU cache（读写一块 > L3 的 dummy，把之前热的数据挤出去）。
// 让每次测量都从「冷 cache」开始，避免 DSA batch 越测越热导致对比不公平。
static void flush_cache(void) {
    static uint8_t *dummy = NULL;
    static const size_t kCache = 256 * 1024 * 1024;  // 256MB > L3（~105MB）
    volatile uint64_t sink = 0;
    if (!dummy) dummy = malloc(kCache);
    for (size_t i = 0; i < kCache; i += 64) {
        dummy[i] = (uint8_t)(i & 0xFF);
    }
    for (size_t i = 0; i < kCache; i += 64) {
        sink += dummy[i];
    }
    (void)sink;
}

static double bench_cpu_4k(uint8_t *src, uint8_t *dst) {
    flush_cache();
    double w0 = wall_time_sec();
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        memcpy(dst + i * BLOCK_SIZE, src + i * BLOCK_SIZE, BLOCK_SIZE);
    }
    double w1 = wall_time_sec();
    return (double)TOTAL_BLOCKS * BLOCK_SIZE / 1e9 / (w1 - w0);
}

static double bench_dsa_batch_4k(uint8_t *src, uint8_t *dst, int n) {
    uint32_t job_size = 0;
    dml_get_job_size(DML_PATH_HW, &job_size);
    dml_job_t *job = malloc(job_size);
    dml_init_job(DML_PATH_HW, job);

    // 必须先设 operation，dml_get_batch_size 依赖它（否则返回 JOB_OPERATION_ERROR）
    job->operation = DML_OP_BATCH;

    uint32_t batch_bytes = 0;
    dml_status_t st = dml_get_batch_size(job, (uint32_t)n, &batch_bytes);
    if (st != DML_STATUS_OK) {
        fprintf(stderr, "dml_get_batch_size(n=%d) failed: %u\n", n, (unsigned)st);
        dml_finalize_job(job);
        free(job);
        return 0.0;
    }
    uint8_t *batch = malloc(batch_bytes);
    job->destination_first_ptr = batch;
    job->destination_length = batch_bytes;

    // n 不整除时余下几块不测，吞吐按实际搬运字节数算，避免 n=48/56/72 时被高估
    int batches = TOTAL_BLOCKS / n;
    flush_cache();
    double w0 = wall_time_sec();
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < n; i++) {
            int idx = b * n + i;
            dml_batch_set_mem_move_by_index(job, (uint32_t)i,
                                            src + idx * BLOCK_SIZE,
                                            dst + idx * BLOCK_SIZE,
                                            BLOCK_SIZE, 0);
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

int main(void) {
    uint8_t *src = malloc((size_t)TOTAL_BLOCKS * BLOCK_SIZE);
    uint8_t *dst = malloc((size_t)TOTAL_BLOCKS * BLOCK_SIZE);
    if (!src || !dst) { perror("malloc"); return 1; }
    memset(src, 0x5A, (size_t)TOTAL_BLOCKS * BLOCK_SIZE);
    memset(dst, 0x00, (size_t)TOTAL_BLOCKS * BLOCK_SIZE);

    double cpu_gbs = 0.0;
    for (int r = 0; r < REPEAT; r++) cpu_gbs += bench_cpu_4k(src, dst);
    cpu_gbs /= REPEAT;
    printf("[CPU 4KB memcpy]       %.2f GB/s (avg of %d)\n", cpu_gbs, REPEAT);

    // 加密采样：重点覆盖阈值附近 48~96，验证 N≈64 的跳变是尖锐阈值还是渐变
    int ns[] = {4, 8, 16, 32, 48, 56, 64, 72, 96, 128, 256};
    for (size_t k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
        int n = ns[k];
        double sum = 0.0;
        printf("[DSA batch %3d x 4KB] ", n);
        for (int r = 0; r < REPEAT; r++) {
            double gbs = bench_dsa_batch_4k(src, dst, n);
            sum += gbs;
            printf("%6.2f%s", gbs, r + 1 < REPEAT ? " |" : "");
        }
        double avg = sum / REPEAT;
        printf("   avg %.2f GB/s (%.2fx CPU)\n", avg, avg / cpu_gbs);
    }

    free(src);
    free(dst);
    return 0;
}
