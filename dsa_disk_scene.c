// dsa_disk_scene.c — 补测: mem2disk / disk2mem 场景的 CPU 成本 + DSA 卸载收益
//
// 背景: 沙箱真正有价值的搬运是 mem2disk(快照生成) 与 disk2mem(快照恢复),
//       不是纯 mem2mem。DSA 只能做「内存↔内存」拷贝, 磁盘 I/O 本身(存储栈 DMA + 文件系统)
//       它碰不到。所以把每条路径拆成两段:
//         - 内存拷贝段 (DSA 可卸载): guest 内存 ↔ 用户 buffer 的 memcpy
//         - 磁盘 I/O 段 (DSA 不可卸载): read()/write() 到文件
//       分别测每段的「每 GB CPU 时间(ms/GB)」, 再拼出两条场景的 CPU 现状 vs DSA 优化后。
//
// 场景组合:
//   disk2mem = 磁盘读(read) + memcpy(buffer → guest)
//   mem2disk = memcpy(guest → buffer) + 磁盘写(write)
//
// 用法:
//   ./dsa_disk_scene [文件路径] [重复次数]
//     文件路径 默认 /tmp/dsa_disk_scene.bin (务必传真实磁盘路径, 见下方注意2)
//     重复次数 默认 15 (每配置重复次数, 影响可信度)
//
// 核心指标: 每 GB CPU 时间 (CLOCK_PROCESS_CPUTIME_ID), DSA 能省 = 拷贝段占比 × 拷贝段 DSA 释放率。
// 可信度: 每段报 avg / stddev / min / max, 不只报平均, 便于判断方差。
//
// 重要注意:
//   1) read/write 走 page cache, 内核的 copy_from/to_user 本身就是 memcpy(计入 CPU 时间)。
//      测「冷盘 wall 吞吐」需 O_DIRECT 或先 drop_caches; 本测聚焦 CPU 成本, 不依赖盘冷热。
//   2) 文件路径务必传真实磁盘路径(如 overlaybd 挂载点 / 本地 NVMe), 否则 /tmp 常是
//      tmpfs(RAM), 盘 I/O 那段的 wall 时间会被 RAM 速度虚高。
//
// 对比基准: 与 dsa_async_offload.c 同口径(2MB 块, busy-poll)。

#include <dml/dml.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#define CHUNK      (2 * 1024 * 1024)   // 2MB 搬运块（单发 DSA 上限 2MB）
#define ITERS      512                  // 512 × 2MB = 1GB
#define DEFAULT_REPEAT 15               // 默认重复次数
#define DEFAULT_PATH   "/tmp/dsa_disk_scene.bin"

typedef struct {
    double avg, sd, min, max;
} Stats;

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

// avg / stddev(样本) / min / max
static Stats stats(const double *v, int n) {
    double *s = malloc((size_t)n * sizeof(double));
    memcpy(s, v, (size_t)n * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (s[j] < s[i]) { double t = s[i]; s[i] = s[j]; s[j] = t; }

    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    Stats st;
    st.avg = sum / n;
    st.min = s[0];
    st.max = s[n - 1];
    double var = 0;
    for (int i = 0; i < n; i++) { double d = v[i] - st.avg; var += d * d; }
    st.sd = sqrt(var / (n - 1));
    free(s);
    return st;
}

// 完整读写(处理 read/write 可能短返回)
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

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : DEFAULT_PATH;
    const int repeat = (argc > 2) ? atoi(argv[2]) : DEFAULT_REPEAT;
    if (repeat < 2) { fprintf(stderr, "重复次数需 >= 2\n"); return 1; }

    const size_t total = (size_t)CHUNK * ITERS;
    const double gb = (double)total / 1e9;

    uint8_t *src = malloc(CHUNK);
    uint8_t *dst = malloc(CHUNK);
    if (!src || !dst) { perror("malloc"); return 1; }
    memset(src, 0x5A, CHUNK);

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    for (int i = 0; i < ITERS; i++)
        if (full_write(fd, src, CHUNK) != CHUNK) { perror("prefill write"); return 1; }
    fsync(fd);

    // ---- 1) CPU memcpy (内存拷贝段基线) ----
    double cpu_ms[repeat], cpu_wall[repeat];
    for (int r = 0; r < repeat; r++) {
        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < ITERS; i++) memcpy(dst, src, CHUNK);
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        cpu_ms[r] = (t1 - t0) / gb * 1000.0; cpu_wall[r] = (w1 - w0) / gb;
    }

    // ---- 2) DSA memcpy (单发 MEM_MOVE, busy-poll, 内存拷贝段优化后) ----
    uint32_t job_size = 0;
    dml_get_job_size(DML_PATH_HW, &job_size);
    dml_job_t *job = malloc(job_size);
    dml_init_job(DML_PATH_HW, job);

    double dsa_ms[repeat], dsa_wall[repeat];
    for (int r = 0; r < repeat; r++) {
        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < ITERS; i++) {
            job->operation = DML_OP_MEM_MOVE;
            job->source_first_ptr = src;
            job->destination_first_ptr = dst;
            job->source_length = CHUNK;
            job->destination_length = CHUNK;
            dml_execute_job(job, DML_WAIT_MODE_BUSY_POLL);
        }
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        dsa_ms[r] = (t1 - t0) / gb * 1000.0; dsa_wall[r] = (w1 - w0) / gb;
    }

    // ---- 3) disk read (disk2mem 的磁盘 I/O 段) ----
    double rd_ms[repeat], rd_wall[repeat];
    for (int r = 0; r < repeat; r++) {
        lseek(fd, 0, SEEK_SET);
        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < ITERS; i++)
            if (full_read(fd, dst, CHUNK) != CHUNK) { perror("read"); return 1; }
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        rd_ms[r] = (t1 - t0) / gb * 1000.0; rd_wall[r] = (w1 - w0) / gb;
    }

    // ---- 4) disk write (mem2disk 的磁盘 I/O 段) ----
    double wr_ms[repeat], wr_wall[repeat];
    for (int r = 0; r < repeat; r++) {
        lseek(fd, 0, SEEK_SET);
        double t0 = cpu_time_sec(), w0 = wall_time_sec();
        for (int i = 0; i < ITERS; i++)
            if (full_write(fd, src, CHUNK) != CHUNK) { perror("write"); return 1; }
        double t1 = cpu_time_sec(), w1 = wall_time_sec();
        wr_ms[r] = (t1 - t0) / gb * 1000.0; wr_wall[r] = (w1 - w0) / gb;
    }

    Stats c = stats(cpu_ms, repeat), d = stats(dsa_ms, repeat);
    Stats r_ = stats(rd_ms, repeat),   w_ = stats(wr_ms, repeat);
    double c_wall = 0, d_wall = 0, r_wall = 0, w_wall = 0;
    for (int i = 0; i < repeat; i++) { c_wall += cpu_wall[i]; d_wall += dsa_wall[i]; r_wall += rd_wall[i]; w_wall += wr_wall[i]; }
    c_wall /= repeat; d_wall /= repeat; r_wall /= repeat; w_wall /= repeat;

    // 场景组合 (每 GB CPU 时间 ms/GB, 基于 avg)
    double d2m_cur = r_.avg + c.avg;   // disk2mem 现状 = 读盘 + CPU memcpy
    double d2m_dsa = r_.avg + d.avg;   // disk2mem DSA  = 读盘 + DSA memcpy
    double m2d_cur = c.avg + w_.avg;   // mem2disk 现状 = CPU memcpy + 写盘
    double m2d_dsa = d.avg + w_.avg;   // mem2disk DSA  = DSA memcpy + 写盘

    printf("# dsa_disk_scene.c — mem2disk / disk2mem 场景 CPU 成本 (每 GB CPU 时间, ms)\n");
    printf("# 方法: CLOCK_PROCESS_CPUTIME_ID; DSA 只卸载「内存拷贝段」, 磁盘 I/O 段不可卸载\n");
    printf("# file = %s, repeat = %d\n", path, repeat);
    printf("segment,avg_cpu_ms_per_gb,stddev_ms,min_ms,max_ms,wall_s_per_gb\n");
    printf("cpu_memcpy,%.1f,%.1f,%.1f,%.1f,%.4f\n", c.avg, c.sd, c.min, c.max, c_wall);
    printf("dsa_memcpy_busypoll,%.1f,%.1f,%.1f,%.1f,%.4f\n", d.avg, d.sd, d.min, d.max, d_wall);
    printf("disk_read,%.1f,%.1f,%.1f,%.1f,%.4f\n", r_.avg, r_.sd, r_.min, r_.max, r_wall);
    printf("disk_write,%.1f,%.1f,%.1f,%.1f,%.4f\n", w_.avg, w_.sd, w_.min, w_.max, w_wall);

    printf("scenario,current_ms_per_gb,dsa_offload_ms_per_gb,savings_pct\n");
    printf("disk2mem,%.1f,%.1f,%.2f\n", d2m_cur, d2m_dsa, (1.0 - d2m_dsa / d2m_cur) * 100.0);
    printf("mem2disk,%.1f,%.1f,%.2f\n", m2d_cur, m2d_dsa, (1.0 - m2d_dsa / m2d_cur) * 100.0);

    printf("\n# 拷贝段 CPU: 现状 %.1f vs DSA %.1f ms/GB (DSA 释放 %.2f%%)\n",
           c.avg, d.avg, (1.0 - d.avg / c.avg) * 100.0);
    printf("# 拷贝段占比: disk2mem %.1f%%, mem2disk %.1f%%\n\n",
           c.avg / d2m_cur * 100.0, c.avg / m2d_cur * 100.0);

    dml_finalize_job(job);
    free(job);
    free(src);
    free(dst);
    close(fd);
    return 0;
}