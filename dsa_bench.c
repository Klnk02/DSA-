// dsa_bench.c — DSA vs 纯软件 onCPU 对比（mem2mem / disk2mem / mem2disk）
// 用途：受控微基准，量化 DSA 相对 CPU 在「数据搬运」上的 onCPU 卸载上限
// 【口径声明】本程序不是 snapshot 等价模拟，只是数据搬运微基准：
//   真实的 AgentENV snapshot 走 io_uring（内核侧异步搬运），DSA 无法介入；
//   本基准测的是「若能介入，DSA 在搬运段能省多少 CPU」的上限。
// 口径：
//   - onCPU = CLOCK_PROCESS_CPUTIME_ID（进程 user+sys CPU 时间）
//   - mem2mem 软件 = memcpy；DSA = DML batch mem_move（UMWAIT 等待，CPU 空闲）
//   - disk2mem 软件 = read() 直读；DSA = read() 到临时缓冲 + DML mem_move 搬入
//   - mem2disk 软件 = write() 直写；DSA = DML mem_move 搬入临时缓冲 + write()
//   - 数据缓冲 4KB 对齐（posix_memalign；DML batch 子任务地址必须页对齐，否则 DSA 报 25）
//   - 磁盘路径须落在真实磁盘挂载点（DSA_BENCH_DIR），tmpfs 会失真（程序会告警）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/vfs.h>
#include <dml/dml.h>

#define TMPFS_MAGIC 0x01021994

#define PG 4096
#define BN 32                          /* batch 子任务数（<= shared wq max_batch_size=32） */
#define GUEST_MEM_MIB 3584             /* 沙箱 guest 内存（标注用，非本程序分配） */

static dml_job_t *g_job;
static uint8_t   *g_batch;
static uint32_t   g_batch_n = BN;

static double cpu_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* DML 硬件路径初始化（成功返回 0） */
static int dsa_init(void)
{
    uint32_t sz = 0;
    if (dml_get_job_size(DML_PATH_HW, &sz) != DML_STATUS_OK || sz == 0) return -1;
    g_job = (dml_job_t *)malloc(sz);
    if (!g_job) return -1;
    if (dml_init_job(DML_PATH_HW, g_job) != DML_STATUS_OK) return -1;
    g_job->operation = DML_OP_BATCH;
    uint32_t bb = 0;
    if (dml_get_batch_size(g_job, g_batch_n, &bb) != DML_STATUS_OK) return -1;
    g_batch = (uint8_t *)malloc(bb);
    if (!g_batch) return -1;
    g_job->destination_first_ptr = g_batch;
    g_job->destination_length    = bb;
    return 0;
}

/* DSA 批量搬运 size 字节（size 须 4KB 对齐；尾页 CPU 兜底） */
static void dsa_memmove(uint8_t *dst, uint8_t *src, size_t size)
{
    uint64_t n = size / PG, batches = n / g_batch_n;
    for (uint64_t b = 0; b < batches; b++) {
        for (uint32_t i = 0; i < g_batch_n; i++) {
            uint64_t k = b * g_batch_n + i;
            dml_batch_set_mem_move_by_index(g_job, i, src + k * PG, dst + k * PG, PG, 0);
        }
        dml_submit_job(g_job);
        dml_wait_job(g_job, DML_WAIT_MODE_UMWAIT);   /* UMWAIT：CPU 真正空闲（卸载口径） */
    }
    for (uint64_t k = batches * g_batch_n; k < n; k++)
        memcpy(dst + k * PG, src + k * PG, PG);
}

static void *align_alloc(size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, PG, size) != 0) return NULL;
    memset(p, 0, size);                 /* touch 建立物理映射 */
    return p;
}

int main(void)
{
    if (dsa_init() != 0) { fprintf(stderr, "DML HW 初始化失败（无 shared wq / libaccel 缺失？）\n"); return 1; }

    const size_t sizes[] = { 64UL << 20, 256UL << 20, 1024UL << 20 };
    const char  *names[] = { "64MB", "256MB", "1GB" };
    const int    reps = 5;

    const char *dir = getenv("DSA_BENCH_DIR");
    if (!dir || !*dir) dir = ".";
    struct statfs sf;                    /* 项目纪律：磁盘路径必须是真实磁盘挂载点 */
    if (statfs(dir, &sf) == 0 && (unsigned long)sf.f_type == TMPFS_MAGIC)
        fprintf(stderr, "WARN: %s 是 tmpfs（内存文件系统）→ disk2mem/mem2disk 测值会失真，"
                        "请设 DSA_BENCH_DIR=<真实磁盘目录>\n", dir);

    printf("# guest_mem=%dMiB  batch=%u  wait=UMWAIT  reps=%d  disk_dir=%s\n",
           GUEST_MEM_MIB, g_batch_n, reps, dir);
    printf("op,size_mb,impl,oncpu_ms,oncpu_ms_per_gb\n");

    for (int s = 0; s < 3; s++) {
        size_t size = sizes[s];
        uint8_t *src = (uint8_t *)align_alloc(size);
        uint8_t *dst = (uint8_t *)align_alloc(size);
        uint8_t *tmp = (uint8_t *)align_alloc(size);   /* disk2mem DSA 方案的临时缓冲 */
        if (!src || !dst || !tmp) { fprintf(stderr, "alloc %s 失败\n", names[s]); return 1; }
        memset(src, 0x5A, size);

        /* ---- mem2mem：memcpy vs DSA ---- */
        double best = 1e18;
        for (int r = 0; r < reps; r++) {
            double t0 = cpu_sec();
            memcpy(dst, src, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (dt < best) best = dt;
        }
        printf("mem2mem,%s,software,%.3f,%.3f\n", names[s], best, best / (size / 1e9));

        best = 1e18;
        for (int r = 0; r < reps; r++) {
            double t0 = cpu_sec();
            dsa_memmove(dst, src, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (dt < best) best = dt;
        }
        printf("mem2mem,%s,dsa,%.3f,%.3f\n", names[s], best, best / (size / 1e9));

        /* ---- disk2mem：read 直读 vs read + DSA 搬入 ----
           cold 口径用 posix_fadvise(DONTNEED) 只逐出本文件自身的缓存，
           不动全局 drop_caches（避免影响共享机上其他租户）。 */
        char fpath[192];
        snprintf(fpath, sizeof fpath, "%s/dsa_bench_%s.bin", dir, names[s]);
        int fd = open(fpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); return 1; }
        if (write(fd, src, size) != (ssize_t)size) { perror("write"); return 1; }
        fsync(fd);

        /* software：read 直读到 dst */
        best = 1e18;
        for (int r = 0; r < reps; r++) {
            posix_fadvise(fd, 0, (off_t)size, POSIX_FADV_DONTNEED);   /* 近似 cold */
            lseek(fd, 0, SEEK_SET);
            double t0 = cpu_sec();
            ssize_t got = read(fd, dst, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (got != (ssize_t)size) { fprintf(stderr, "read 失败\n"); return 1; }
            if (dt < best) best = dt;
        }
        printf("disk2mem,%s,software,%.3f,%.3f\n", names[s], best, best / (size / 1e9));

        /* dsa：read 到 tmp + DSA 搬入 dst */
        best = 1e18;
        for (int r = 0; r < reps; r++) {
            posix_fadvise(fd, 0, (off_t)size, POSIX_FADV_DONTNEED);
            lseek(fd, 0, SEEK_SET);
            double t0 = cpu_sec();
            ssize_t got = read(fd, tmp, size);
            dsa_memmove(dst, tmp, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (got != (ssize_t)size) { fprintf(stderr, "read 失败\n"); return 1; }
            if (dt < best) best = dt;
        }
        printf("disk2mem,%s,dsa,%.3f,%.3f\n", names[s], best, best / (size / 1e9));

        /* ---- mem2disk：write 直写 vs DSA 搬 + write ----
           onCPU 主体 = write 的 copy_from_user（应用 buffer → page cache），
           DSA 搬 src→tmp 近乎 0 CPU 但省不了 copy_from_user 那段。 */
        best = 1e18;
        for (int r = 0; r < reps; r++) {
            ftruncate(fd, 0);
            lseek(fd, 0, SEEK_SET);
            double t0 = cpu_sec();
            ssize_t got = write(fd, src, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (got != (ssize_t)size) { fprintf(stderr, "write 失败\n"); return 1; }
            if (dt < best) best = dt;
        }
        printf("mem2disk,%s,software,%.3f,%.3f\n", names[s], best, best / (size / 1e9));

        best = 1e18;
        for (int r = 0; r < reps; r++) {
            ftruncate(fd, 0);
            lseek(fd, 0, SEEK_SET);
            double t0 = cpu_sec();
            dsa_memmove(tmp, src, size);
            ssize_t got = write(fd, tmp, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (got != (ssize_t)size) { fprintf(stderr, "write 失败\n"); return 1; }
            if (dt < best) best = dt;
        }
        printf("mem2disk,%s,dsa,%.3f,%.3f\n", names[s], best, best / (size / 1e9));

        close(fd);
        unlink(fpath);
        free(src); free(dst); free(tmp);
    }

    dml_finalize_job(g_job);
    free(g_job); free(g_batch);
    return 0;
}
