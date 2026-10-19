// dsa_min.c — 用 malloc 复刻 replay 的 batch 场景，失败则读子任务真实状态码
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dml/dml.h>

#define PG 4096
#define BN 32
#define SZ (64UL * 1024 * 1024)

static dml_job_t *g_job;

int main(void)
{
    uint32_t js = 0;
    dml_get_job_size(DML_PATH_HW, &js);
    g_job = (dml_job_t *)malloc(js);
    dml_init_job(DML_PATH_HW, g_job);

    uint8_t *src = (uint8_t *)malloc(SZ);   /* 模拟 replay 的 g_lbuf */
    uint8_t *dst = (uint8_t *)malloc(SZ);   /* 模拟 replay 的 g_dst */
    memset(src, 0x5A, SZ);                  /* 模拟 prefetch 写 g_lbuf */
    memset(dst, 0x00, SZ);                  /* 模拟 dst 预热 */
    printf("src=%p (末%03lx) dst=%p (末%03lx)\n",
           (void *)src, (unsigned long)((uintptr_t)src & 0xfff),
           (void *)dst, (unsigned long)((uintptr_t)dst & 0xfff));

    uint32_t bb = 0;
    g_job->operation = DML_OP_BATCH;
    dml_status_t st = dml_get_batch_size(g_job, BN, &bb);
    printf("[1] get_batch_size = %u (bytes=%u)\n", (unsigned)st, bb);

    uint8_t *batch_buf = (uint8_t *)malloc(bb);   /* heap，和 replay 的 g_batch 一致 */
    g_job->destination_first_ptr = batch_buf;
    g_job->destination_length    = bb;

    for (uint32_t i = 0; i < BN; i++) {
        st = dml_batch_set_mem_move_by_index(g_job, i,
                src + (size_t)i * PG, dst + (size_t)i * PG, PG, 0);
        if (st != DML_STATUS_OK) { printf("[2] set[%u] = %u\n", i, (unsigned)st); return 1; }
    }
    printf("[2] 32 子任务设置成功\n");

    st = dml_submit_job(g_job);
    printf("[3] submit = %u\n", (unsigned)st);

    st = dml_wait_job(g_job, DML_WAIT_MODE_BUSY_POLL);
    printf("[4] wait(batch malloc64MB) = %u\n", (unsigned)st);

    if (st != DML_STATUS_OK) {
        for (uint32_t i = 0; i < BN; i++) {
            dml_status_t sub = DML_STATUS_OK;
            dml_batch_get_status(g_job, i, &sub);
            if (sub != DML_STATUS_OK)
                printf("    task[%u] = %u   src=%p dst=%p\n",
                       i, (unsigned)sub,
                       (void *)(src + (size_t)i * PG),
                       (void *)(dst + (size_t)i * PG));
        }
    }

    dml_finalize_job(g_job);
    free(g_job);
    return 0;
}