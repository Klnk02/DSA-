// dsa_offload.cc — DSA 卸载 memcpy 的 CPU 时间对比（mem2mem 摸底）
//
// 目的：量化「把 memcpy 从 CPU 卸载到 DSA 后，CPU 花在搬运上的时间变化」。
//
// 方法：搬运同样数据量，对比「CPU 路径(glibc memcpy)」与「DSA 路径(dml::execute)」
//       各自消耗的 CPU 时间(clock_gettime CLOCK_PROCESS_CPUTIME_ID)。
//       CPU 时间节省率 = (CPU路径CPU时间 - DSA路径CPU时间) / CPU路径CPU时间。
//
// 注意：DML 高层 API 的 dedicated 队列是同步 busy-poll（CPU 提交后忙等完成），
//       因此本程序测出的「节省率」本质来自 DSA 吞吐比 CPU 快（大块 memcpy 约 1.9×），
//       而非 CPU 真正空闲下来去干别的。真异步释放需 shared 队列 + ENQCMD，留阶段 3。

#include <dml/dml.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <vector>

namespace {

using namespace std::chrono;

// 进程 CPU 时间（秒，纳秒精度）
double cpu_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

}  // namespace

int main() {
    constexpr size_t kSize = 2 * 1024 * 1024;  // 单次 2MB（DSA 单发上限内）
    constexpr int kIters = 2000;               // 总数据量 4GB
    std::vector<uint8_t> src(kSize, 0x5A);
    std::vector<uint8_t> dst(kSize, 0x00);

    const double total_gb = static_cast<double>(kSize) * kIters / 1e9;
    double cpu_per_gb = 0.0;
    double dsa_per_gb = 0.0;

    // ---- CPU 路径 ----
    {
        const double t0 = cpu_time_sec();
        const auto w0 = steady_clock::now();
        for (int i = 0; i < kIters; ++i) {
            std::memcpy(dst.data(), src.data(), kSize);
            asm volatile("" ::: "memory");  // 防编译器优化掉写操作
        }
        const auto w1 = steady_clock::now();
        const double t1 = cpu_time_sec();

        const double wall = duration_cast<microseconds>(w1 - w0).count() / 1e6;
        const double cpu = t1 - t0;
        cpu_per_gb = cpu / total_gb;
        std::cout << "[CPU memcpy] wall=" << wall << "s  cpu=" << cpu << "s"
                  << "  占用率=" << (cpu / wall * 100.0) << "%"
                  << "  每GB CPU时间=" << cpu_per_gb << "s\n";
    }

    // ---- DSA 路径 ----
    {
        const double t0 = cpu_time_sec();
        const auto w0 = steady_clock::now();
        for (int i = 0; i < kIters; ++i) {
            auto result = dml::execute<dml::hardware>(
                dml::mem_move,
                dml::make_view(src.data(), src.size()),
                dml::make_view(dst.data(), dst.size()));
            (void)result;
            asm volatile("" ::: "memory");
        }
        const auto w1 = steady_clock::now();
        const double t1 = cpu_time_sec();

        const double wall = duration_cast<microseconds>(w1 - w0).count() / 1e6;
        const double cpu = t1 - t0;
        dsa_per_gb = cpu / total_gb;
        std::cout << "[DSA memmove] wall=" << wall << "s  cpu=" << cpu << "s"
                  << "  占用率=" << (cpu / wall * 100.0) << "%"
                  << "  每GB CPU时间=" << dsa_per_gb << "s\n";
    }

    // ---- CPU 时间节省率（同步 busy-poll，非异步释放）----
    std::cout << "----------------------------------------\n";
    std::cout << "CPU 时间节省率 = " << (1.0 - dsa_per_gb / cpu_per_gb) * 100.0 << "%"
              << "  （同步 busy-poll 下的节省，非异步释放；异步释放需 shared+ENQCMD，留阶段 3）\n";

    return 0;
}