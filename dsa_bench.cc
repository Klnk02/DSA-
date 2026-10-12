// dsa_bench.cc — Intel DSA vs CPU 数据搬运基准测试（mem2mem 摸底）
//
// 用途：量化 DSA（Data Streaming Accelerator）卸载 memcpy/memset 的收益。
// 这是「鲲鹏 data stream 加速器」预研中，基于 Intel 服务器摸底收益的第一步（mem2mem）。
//
// 对比三种执行路径：
//   1. glibc memcpy/memset   —— CPU 基线（100% CPU 搬运）
//   2. DML software path     —— CPU 上的 DML 软件实现（对照）
//   3. DML hardware path     —— DSA 硬件卸载（目标收益来源）
//
// 输出：Google Benchmark 的 CSV（含 real_time / cpu_time / bytes_per_second）。
//
// 重要说明：
//   - DSA hardware 路径用异步 dml::submit + handler.get() 显式等待完成，
//     测的是真实「搬运吞吐」；get() 内部 busy-poll 等 DSA 完成，故 cpu_time 仍 ≈ real_time。
//   - CPU 释放率（提交后 CPU 空出来干别的）需异步 submit 且「不轮询」才能体现，
//     那是后续 dsa_cpu_offload.cc 的测法。

#include <benchmark/benchmark.h>
#include <dml/dml.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// 单次 DSA 操作的最大搬运量受 wq 的 max-transfer-size 限制（setup_dsa.sh 设为 2MB）。
// 因此 hardware path 的 size 上限取 2MB（1<<21），CPU path 保持同样范围以便公平对比。
constexpr int64_t kMinSize = 1 << 12;  // 4KB
constexpr int64_t kMaxSize = 1 << 21;  // 2MB

// ---- memcpy：CPU (glibc) ----
void BM_Memcpy_CPU(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> src(size);
    std::vector<uint8_t> dst(size);
    std::memset(src.data(), 0x5A, size);
    std::memset(dst.data(), 0x00, size);

    for (auto _ : state) {
        std::memcpy(dst.data(), src.data(), size);
        benchmark::DoNotOptimize(dst.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

// ---- memcpy：DML software ----
void BM_Memcpy_DML_SW(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> src(size);
    std::vector<uint8_t> dst(size);
    std::memset(src.data(), 0x5A, size);
    std::memset(dst.data(), 0x00, size);

    for (auto _ : state) {
        auto result = dml::execute<dml::software>(
            dml::mem_move,
            dml::make_view(src.data(), src.size()),
            dml::make_view(dst.data(), dst.size()));
        benchmark::DoNotOptimize(result.status);
        benchmark::DoNotOptimize(dst.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

// ---- memcpy：DSA hardware（异步 submit + 显式等完成）----
void BM_Memcpy_DSA_HW(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> src(size);
    std::vector<uint8_t> dst(size);
    std::memset(src.data(), 0x5A, size);
    std::memset(dst.data(), 0x00, size);

    // 计时外做一次完整性校验：确认 submit + get 后数据真正落地
    {
        auto handler = dml::submit<dml::hardware>(
            dml::mem_move,
            dml::make_view(src.data(), src.size()),
            dml::make_view(dst.data(), dst.size()));
        auto result = handler.get();
        if (result.status != dml::status_code::ok ||
            std::memcmp(src.data(), dst.data(), size) != 0) {
            state.SkipWithError("DSA mem_move 失败或数据不一致");
            return;
        }
    }

    for (auto _ : state) {
        auto handler = dml::submit<dml::hardware>(
            dml::mem_move,
            dml::make_view(src.data(), src.size()),
            dml::make_view(dst.data(), dst.size()));
        auto result = handler.get();
        benchmark::DoNotOptimize(result.status);
        benchmark::DoNotOptimize(dst.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

// ---- fill：CPU (memset) ----
void BM_Fill_CPU(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> dst(size, 0);

    for (auto _ : state) {
        std::memset(dst.data(), 0x5A, size);
        benchmark::DoNotOptimize(dst.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

// ---- fill：DML software ----
void BM_Fill_DML_SW(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> dst(size, 0);
    const uint64_t pattern = 0x0102030405060708ULL;

    for (auto _ : state) {
        auto result = dml::execute<dml::software>(
            dml::fill,
            pattern,
            dml::make_view(dst.data(), dst.size()));
        benchmark::DoNotOptimize(result.status);
        benchmark::DoNotOptimize(dst.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

// ---- fill：DSA hardware（异步 submit + 显式等完成）----
void BM_Fill_DSA_HW(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> dst(size, 0);
    const uint64_t pattern = 0x0102030405060708ULL;

    // 计时外校验一次：确认 submit + get 成功
    {
        auto handler = dml::submit<dml::hardware>(
            dml::fill,
            pattern,
            dml::make_view(dst.data(), dst.size()));
        auto result = handler.get();
        if (result.status != dml::status_code::ok) {
            state.SkipWithError("DSA fill 失败");
            return;
        }
    }

    for (auto _ : state) {
        auto handler = dml::submit<dml::hardware>(
            dml::fill,
            pattern,
            dml::make_view(dst.data(), dst.size()));
        auto result = handler.get();
        benchmark::DoNotOptimize(result.status);
        benchmark::DoNotOptimize(dst.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(size));
}

}  // namespace

BENCHMARK(BM_Memcpy_CPU)->RangeMultiplier(2)->Range(kMinSize, kMaxSize);
BENCHMARK(BM_Memcpy_DML_SW)->RangeMultiplier(2)->Range(kMinSize, kMaxSize);
BENCHMARK(BM_Memcpy_DSA_HW)->RangeMultiplier(2)->Range(kMinSize, kMaxSize);
BENCHMARK(BM_Fill_CPU)->RangeMultiplier(2)->Range(kMinSize, kMaxSize);
BENCHMARK(BM_Fill_DML_SW)->RangeMultiplier(2)->Range(kMinSize, kMaxSize);
BENCHMARK(BM_Fill_DSA_HW)->RangeMultiplier(2)->Range(kMinSize, kMaxSize);

BENCHMARK_MAIN();
