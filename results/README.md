# results/ — 各阶段结果留痕

本目录存放每个阶段的实测产出。原则：**原始数据与结果进文件，主 [README](../README.md) 只留结论摘要与指针**。

| 目录 | 对应阶段 | 内容 |
|---|---|---|
| `stage0-env/` | 阶段 0 环境探测 | `check_dsa_env.sh` 的输出 |
| `stage1-config/` | 阶段 1 启用配置 | `setup_dsa.sh` 的执行日志 |
| `stage2-benchmark/` | 阶段 2 核心基准 | CPU vs DSA 吞吐矩阵、盈亏平衡、CPU 释放率 |
| `stage3-async/` | 阶段 3 异步释放 | 真异步卸载的 CPU 释放测量 |
| `stage3-batch/` | 阶段 3 Batch 打包 | 4KB 页操作批量提交测试 |
| `stage4-sandbox/` | 阶段 4 沙箱集成 | （待产出） |
| `stage5-report/` | 阶段 5 结论报告 | （待产出） |

## 已产出

- `stage2-benchmark/dsa_bench_results.csv` — `dsa_bench.cc` 原始 benchmark 输出（2026-10-12，8480+，含 6 组 × 10 尺寸的延迟/CPU时间/吞吐，元信息以 `#` 注释保留）。
- `stage2-benchmark/memcpy_fill_cpu_vs_dsa.csv` — 从原始结果提炼的 CPU vs DSA 吞吐对比矩阵（单位 GiB/s，易读易画图）。
- `stage2-benchmark/cpu_time_offload.csv` — `dsa_offload.cc` 实测（吞吐/占用率/每 GB CPU 时间），佐证大块吞吐 1.92× 及「同步 busy-poll 不释放」。
- `stage3-async/cpu_offload_async.csv` — `dsa_async_offload.c` 实测的四种模式 CPU 占用对比（异步释放率 89.76%，umwait 只省电不释放）。
- `stage3-batch/batch_4k.csv` — `dsa_batch_4k.c` 实测的 4KB 页操作 Batch 打包吞吐（存在批量效率阈值 N≈64，N≥64 反超 CPU 5~8 倍）。
