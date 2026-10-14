# results/ — 各阶段结果留痕

本目录存放每个阶段的实测产出。原则：**原始数据与结果进文件，主 [README](../README.md) 只留结论摘要与指针**。

| 目录 | 对应阶段 | 内容 |
|---|---|---|
| `stage0-env/` | 阶段 0 环境探测 | `check_dsa_env.sh` 的输出 |
| `stage1-config/` | 阶段 1 启用配置 | `setup_dsa.sh` 的执行日志 |
| `stage2-benchmark/` | 阶段 2 核心基准 | CPU vs DSA 吞吐矩阵、盈亏平衡、CPU 释放率 |
| `stage3-async/` | 阶段 3 异步释放 | 真异步卸载的 CPU 释放测量 |
| `stage3-batch/` | 阶段 3 Batch 打包 | 4KB 页操作批量提交测试 |
| `stage4-scene/` | 阶段 4.1 场景建模 | 散落页 batch + 页温度 |
| `stage4-sandbox/` | 阶段 4.2 沙箱搬运路径 | AgentENV snapshot memory 搬运链路量化 |
| `stage5-report/` | 阶段 5 结论报告 | （待产出） |

## 已产出

- `stage2-benchmark/dsa_bench_results.csv` — `dsa_bench.cc` 原始 benchmark 输出（2026-10-12，8480+，含 6 组 × 10 尺寸的延迟/CPU时间/吞吐，元信息以 `#` 注释保留）。
- `stage2-benchmark/memcpy_fill_cpu_vs_dsa.csv` — 从原始结果提炼的 CPU vs DSA 吞吐对比矩阵（单位 GiB/s，易读易画图）。
- `stage2-benchmark/cpu_time_offload.csv` — `dsa_offload.cc` 实测（吞吐/占用率/每 GB CPU 时间），佐证大块吞吐 1.92× 及「同步 busy-poll 不释放」。
- `stage3-async/cpu_offload_async.csv` — `dsa_async_offload.c` 实测的四种模式 CPU 占用对比（异步释放率 89.76%，umwait 只省电不释放）。
- `stage3-batch/batch_4k.csv` — `dsa_batch_4k.c` 实测的 4KB 页操作 Batch 打包吞吐（存在尖锐批量效率阈值 N≈48，N≥48 反超 CPU 5~11 倍）。
- `stage4-scene/scene_4k.csv` — `dsa_scene_4k.c` 实测（散落页 batch 物理前提成立；页温度决定 5~11× 仅在冷页成立，热页缩水到 1~2.5×）。
- `stage4-sandbox/snapshot_offload.csv` — AgentENV snapshot memory 搬运路径实测（搬运 CPU 942ms/GB；链路 = `process_vm_readv` 读 guest dirty 页 → `pwrite` 写 OverlayBD commit 文件；符号构成：ext4 簿记 ~55% / copy_page_to_iter 10.18% / copy_page_from_iter_atomic 2.46%；DSA 卸载 CPU 下降：4KB busy-poll +78.4%、64KB +91%、4KB usleep 是假象）。
- `stage4-sandbox/baseline_sweep.csv` — A 臂基线扫描原始输出（2026-10-14，0/64/256MB × repeat=20，1024/2048 待补；统计口径：剔除 perf record 的 rep 1/8/15（profiling 使 wall +2.2s）与 64MB rep4（漏 dirty），结论见主 README 4.4）。
