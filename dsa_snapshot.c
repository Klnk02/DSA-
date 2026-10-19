// dsa_snapshot.c — 真实 AgentENV 沙箱 snapshot 穿刺：mem2mem / disk2mem / mem2disk 的 DSA vs 软件 onCPU
// 用法（root）：
//   ./dsa_snapshot --fc-pid <pid> --disk-file /dev/ublkb4 --out-file /tmp/snap.bin --size-mb 64
//   --fc-pid    guest firecracker 进程（mem2mem 源 = guest RAM 可读大段前 N MB）
//   --disk-file 增量层块设备（disk2mem 源，只读）
//   --out-file  快照文件（mem2disk 目的）
//   --size-mb   搬运大小 = snapshot 大小档（64/256/1024）
// 口径：
//   - onCPU = CLOCK_PROCESS_CPUTIME_ID（进程 user+sys CPU）
//   - mem2mem  软件 = memcpy；DSA = DML batch mem_move（UMWAIT）
//   - disk2mem 软件 = read() 直读；DSA = read() 到临时 + DML 搬入
//   - mem2disk 软件 = write() 直写；DSA = DML 搬入临时 + write()
//   - 数据缓冲 4KB 对齐（DML batch 子任务地址必须页对齐）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <dml/dml.h>

#define PG 4096
#define BN 32

static dml_job_t *g_job;
static uint8_t   *g_batch;
static uint32_t   g_batch_n = BN;

static double cpu_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

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

static void dsa_memmove(uint8_t *dst, uint8_t *src, size_t size)
{
    uint64_t n = size / PG, batches = n / g_batch_n;
    for (uint64_t b = 0; b < batches; b++) {
        for (uint32_t i = 0; i < g_batch_n; i++) {
            uint64_t k = b * g_batch_n + i;
            dml_batch_set_mem_move_by_index(g_job, i, src + k * PG, dst + k * PG, PG, 0);
        }
        dml_submit_job(g_job);
        dml_wait_job(g_job, DML_WAIT_MODE_UMWAIT);
    }
    for (uint64_t k = batches * g_batch_n; k < n; k++)
        memcpy(dst + k * PG, src + k * PG, PG);
}

static void *align_alloc(size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, PG, size) != 0) return NULL;
    memset(p, 0, size);
    return p;
}

/* 找 guest 可读大段（真实 RAM 近似），返回首地址与大小 */
static int find_guest_ram(pid_t pid, uint64_t *addr, uint64_t *size)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    uint64_t best_s = 0, best_e = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e, off, ino;
        char perms[8], dev[64];
        if (sscanf(line, "%lx-%lx %7s %lx %63s %lu", &s, &e, perms, &off, dev, &ino) < 6) continue;
        if (perms[0] != 'r') continue;
        if (e - s > best_e - best_s) { best_s = s; best_e = e; }
    }
    fclose(f);
    if (best_e <= best_s) return -1;
    *addr = best_s;
    *size = best_e - best_s;
    return 0;
}

static int read_guest(pid_t pid, uint64_t hva, uint8_t *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        struct iovec liov = { buf + done, size - done };
        struct iovec riov = { (void *)(uintptr_t)(hva + done), size - done };
        ssize_t r = process_vm_readv(pid, &liov, 1, &riov, 1, 0);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static void emit(const char *op, const char *size_tag, const char *impl, double best_ms, size_t size)
{
    printf("%s,%s,%s,%.3f,%.3f\n", op, size_tag, impl, best_ms, best_ms / (size / 1e9));
}

int main(int argc, char **argv)
{
    pid_t fc_pid = 0;
    const char *disk_file = NULL, *out_file = NULL;
    size_t size_mb = 64;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fc-pid") && i + 1 < argc) fc_pid = (pid_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--disk-file") && i + 1 < argc) disk_file = argv[++i];
        else if (!strcmp(argv[i], "--out-file") && i + 1 < argc) out_file = argv[++i];
        else if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) size_mb = (size_t)atoi(argv[++i]);
    }
    if (size_mb == 0) { fprintf(stderr, "用法见文件头注释\n"); return 1; }
    if (dsa_init() != 0) { fprintf(stderr, "DML HW 初始化失败\n"); return 1; }

    size_t size = size_mb << 20;
    char stag[32];
    snprintf(stag, sizeof stag, "%zuMB", size_mb);
    const int reps = 5;

    uint8_t *lbuf = (uint8_t *)align_alloc(size);   /* mem2mem 源快照 / disk2mem 临时 */
    uint8_t *dst  = (uint8_t *)align_alloc(size);
    uint8_t *tmp  = (uint8_t *)align_alloc(size);
    if (!lbuf || !dst || !tmp) { fprintf(stderr, "alloc 失败\n"); return 1; }

    printf("# guest_pid=%d  snapshot_size=%zuMB  batch=%u  wait=UMWAIT  reps=%d\n",
           (int)fc_pid, size_mb, g_batch_n, reps);
    printf("op,size,impl,oncpu_ms,oncpu_ms_per_gb\n");

    double best;

    /* ---- mem2mem：guest RAM → 内存 ----
       先 process_vm_readv 读 guest 可读大段前 N MB 到 lbuf（快照），再 memcpy/DML 搬入 dst。 */
    if (fc_pid > 0) {
        uint64_t ga = 0, gs = 0;
        if (find_guest_ram(fc_pid, &ga, &gs) != 0 || gs < size) {
            fprintf(stderr, "WARN: guest RAM 可读段 < %zuMB，跳过 mem2mem\n", size_mb);
        } else {
            if (read_guest(fc_pid, ga, lbuf, size) != 0) { fprintf(stderr, "读 guest RAM 失败\n"); return 1; }

            best = 1e18;
            for (int r = 0; r < reps; r++) {
                double t0 = cpu_sec();
                memcpy(dst, lbuf, size);
                double dt = (cpu_sec() - t0) * 1e3;
                if (dt < best) best = dt;
            }
            emit("mem2mem", stag, "software", best, size);

            best = 1e18;
            for (int r = 0; r < reps; r++) {
                double t0 = cpu_sec();
                dsa_memmove(dst, lbuf, size);
                double dt = (cpu_sec() - t0) * 1e3;
                if (dt < best) best = dt;
            }
            emit("mem2mem", stag, "dsa", best, size);
        }
    } else {
        fprintf(stderr, "WARN: 未给 --fc-pid，跳过 mem2mem\n");
    }

    /* ---- disk2mem：增量层 → 内存 ---- */
    if (disk_file) {
        int fd = open(disk_file, O_RDONLY);
        if (fd < 0) { perror("open disk-file"); return 1; }
        struct stat st;
        size_t disk_size = size;
        if (fstat(fd, &st) == 0 && (uint64_t)st.st_size < size) {
            disk_size = st.st_size & ~(size_t)(PG - 1);   /* 增量层常不足 1GB：按实际大小 4KB 对齐 */
            if (disk_size < PG) {
                fprintf(stderr, "WARN: 增量层文件过小（%lldB），跳过 disk2mem\n", (long long)st.st_size);
                close(fd);
            } else {
                fprintf(stderr, "WARN: 增量层仅 %lldB，disk2mem 按 %zuMB 测\n",
                        (long long)st.st_size, disk_size >> 20);
            }
        }
        if (disk_size >= PG) {
            best = 1e18;
            for (int r = 0; r < reps; r++) {
                lseek(fd, 0, SEEK_SET);
                double t0 = cpu_sec();
                ssize_t got = read(fd, dst, disk_size);
                double dt = (cpu_sec() - t0) * 1e3;
                if (got != (ssize_t)disk_size) { fprintf(stderr, "read 不足\n"); return 1; }
                if (dt < best) best = dt;
            }
            emit("disk2mem", stag, "software", best, disk_size);

            best = 1e18;
            for (int r = 0; r < reps; r++) {
                lseek(fd, 0, SEEK_SET);
                double t0 = cpu_sec();
                ssize_t got = read(fd, tmp, disk_size);
                dsa_memmove(dst, tmp, disk_size);
                double dt = (cpu_sec() - t0) * 1e3;
                if (got != (ssize_t)disk_size) { fprintf(stderr, "read 不足\n"); return 1; }
                if (dt < best) best = dt;
            }
            emit("disk2mem", stag, "dsa", best, disk_size);
        }
        close(fd);
    } else {
        fprintf(stderr, "WARN: 未给 --disk-file，跳过 disk2mem\n");
    }

    /* ---- mem2disk：内存 → 快照文件 ---- */
    if (out_file) {
        memset(lbuf, 0x5A, size);
        int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open out-file"); return 1; }
        best = 1e18;
        for (int r = 0; r < reps; r++) {
            lseek(fd, 0, SEEK_SET);
            double t0 = cpu_sec();
            ssize_t got = write(fd, lbuf, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (got != (ssize_t)size) { fprintf(stderr, "write 不足\n"); return 1; }
            if (dt < best) best = dt;
        }
        emit("mem2disk", stag, "software", best, size);

        best = 1e18;
        for (int r = 0; r < reps; r++) {
            lseek(fd, 0, SEEK_SET);
            double t0 = cpu_sec();
            dsa_memmove(tmp, lbuf, size);
            ssize_t got = write(fd, tmp, size);
            double dt = (cpu_sec() - t0) * 1e3;
            if (got != (ssize_t)size) { fprintf(stderr, "write 不足\n"); return 1; }
            if (dt < best) best = dt;
        }
        emit("mem2disk", stag, "dsa", best, size);
        close(fd);
    } else {
        fprintf(stderr, "WARN: 未给 --out-file，跳过 mem2disk\n");
    }

    dml_finalize_job(g_job);
    free(g_job); free(g_batch);
    free(lbuf); free(dst); free(tmp);
    return 0;
}
