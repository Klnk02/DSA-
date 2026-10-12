// dsa_async_offload.c — DSA 真异步卸载的 CPU 释放测量（阶段 3 核心 KPI）
//
// 用低层 C API 的异步接口（dml_submit_job）测「提交后 CPU 不再 busy-poll」时，
// 搬运任务占用的 CPU 时间能降到多少——这才是「释放 vCPU」的正确量化。
//
// 对比四种模式搬运同样 4GB（2MB × 2000）：
//   1. CPU memcpy（glibc，同步，占用 ~100%）
//   2. DSA 同步 busy-poll（dml_execute_job + DML_WAIT_MODE_BUSY_POLL）
//   3. DSA 异步 + sleep（dml_submit_job → usleep 让出 → dml_wait_job）
//   4. DSA 异步 + umwait（dml_submit_job → dml_wait_job DML_WAIT_MODE_UMWAIT）
//
// 核心指标：每 GB 数据的 CPU 时间（CPU time / GB）。CPU 释放率 =
//   (CPU路径每GB CPU时间 - DSA异步路径每GB CPU时间) / CPU路径每GB CPU时间。

#include <dml/dml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static void fill_mem_move(dml_job_t *job, uint8_t *src, uint8_t *dst, uint32_t size) {
    job->operation = DML_OP_MEM_MOVE;
    job->source_first_ptr = src;
    job->destination_first_ptr = dst;
    job->source_length = size;
    job->destination_length = size;
}

int main(void) {
    const uint32_t size = 2 * 1024 * 1024;  // 2MB
    const int iters = 2000;                 // 4GB
    uint8_t *src = malloc(size);
    uint8_t *dst = malloc(size);
    if (!src || !dst) { perror("malloc"); return 1; }
    memset(src, 0x5A, size);
    memset(dst, 0x00, size);

    const double total_gb = (double)size * iters / 1e9;
    double cpu_cpu_per_gb = 0.0, dsa_cpu_per_gb = 0.0;

    // 1. CPU memcpy
    {
        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < iters; i++) {
            memcpy(dst, src, size);
            __asm__ volatile("" ::: "memory");
        }
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        double cpu = t1 - t0, wall = w1 - w0;
        cpu_cpu_per_gb = cpu / total_gb;
        printf("[1 CPU memcpy]        wall=%.4fs cpu=%.4fs 占用率=%.2f%% 每GB CPU时间=%.4fs\n",
               wall, cpu, cpu / wall * 100.0, cpu_cpu_per_gb);
    }

    // 2. DSA 同步 busy-poll
    {
        uint32_t job_size = 0;
        dml_get_job_size(DML_PATH_HW, &job_size);
        dml_job_t *job = malloc(job_size);
        dml_init_job(DML_PATH_HW, job);

        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < iters; i++) {
            fill_mem_move(job, src, dst, size);
            dml_execute_job(job, DML_WAIT_MODE_BUSY_POLL);
        }
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        double cpu = t1 - t0, wall = w1 - w0;
        printf("[2 DSA sync busy-poll] wall=%.4fs cpu=%.4fs 占用率=%.2f%% 每GB CPU时间=%.4fs\n",
               wall, cpu, cpu / wall * 100.0, cpu / total_gb);

        dml_finalize_job(job);
        free(job);
    }

    // 3. DSA 异步 + sleep（模拟 CPU 让出去干别的）
    {
        uint32_t job_size = 0;
        dml_get_job_size(DML_PATH_HW, &job_size);
        dml_job_t *job = malloc(job_size);
        dml_init_job(DML_PATH_HW, job);

        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < iters; i++) {
            fill_mem_move(job, src, dst, size);
            dml_submit_job(job);            // 异步提交，立即返回
            usleep(150);                    // CPU 睡眠，DSA 后台搬（2MB ~75us）
            dml_wait_job(job, DML_WAIT_MODE_BUSY_POLL);  // 已搬完，立即返回
        }
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        double cpu = t1 - t0, wall = w1 - w0;
        dsa_cpu_per_gb = cpu / total_gb;
        printf("[3 DSA async+sleep]   wall=%.4fs cpu=%.4fs 占用率=%.2f%% 每GB CPU时间=%.4fs\n",
               wall, cpu, cpu / wall * 100.0, dsa_cpu_per_gb);

        dml_finalize_job(job);
        free(job);
    }

    // 4. DSA 异步 + umwait（低功耗等待）
    {
        uint32_t job_size = 0;
        dml_get_job_size(DML_PATH_HW, &job_size);
        dml_job_t *job = malloc(job_size);
        dml_init_job(DML_PATH_HW, job);

        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < iters; i++) {
            fill_mem_move(job, src, dst, size);
            dml_submit_job(job);
            dml_wait_job(job, DML_WAIT_MODE_UMWAIT);  // umwait 低功耗等待
        }
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        double cpu = t1 - t0, wall = w1 - w0;
        printf("[4 DSA async+umwait]  wall=%.4fs cpu=%.4fs 占用率=%.2f%% 每GB CPU时间=%.4fs\n",
               wall, cpu, cpu / wall * 100.0, cpu / total_gb);

        dml_finalize_job(job);
        free(job);
    }

    printf("----------------------------------------\n");
    printf("CPU 释放率（async+sleep vs CPU）= %.2f%%\n",
           (1.0 - dsa_cpu_per_gb / cpu_cpu_per_gb) * 100.0);

    free(src);
    free(dst);
    return 0;
}
