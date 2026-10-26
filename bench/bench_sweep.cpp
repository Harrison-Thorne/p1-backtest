// 参数扫描的扩展性：同一批回测任务，分别用 1、2、4、逻辑核数、2×逻辑核数个 worker 跑，
// 每种跑 --reps 次取中位数；另跑一次「不开线程池、顺序执行」作为基准，并检查所有结果完全一致。
// 用法: bench_sweep [--reps 5] [--out results] <导出目录>...
#include "bt/sweep.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace {

double seconds_of(const std::vector<const bt::Dataset*>& ds, const std::vector<bt::Params>& grid, int workers,
                  std::vector<bt::SweepResult>* out) {
    const auto start = std::chrono::steady_clock::now();
    *out = bt::run_sweep(ds, grid, workers);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// 并行结果必须和顺序结果逐个相同：任务之间如果有数据竞争，这里最先暴露
bool same_results(const std::vector<bt::SweepResult>& a, const std::vector<bt::SweepResult>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].result.final_equity != b[i].result.final_equity || a[i].result.closes != b[i].result.closes) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int reps = 5;
    std::string out_dir = "results";
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--reps" && i + 1 < argc) {
            reps = std::stoi(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else {
            dirs.push_back(arg);
        }
    }
    if (dirs.empty()) {
        std::cerr << "usage: bench_sweep [--reps N] [--out dir] <dataset dir>...\n";
        return 2;
    }

    std::vector<bt::Dataset> datasets;
    for (const std::string& d : dirs) datasets.push_back(bt::load_dataset(d));
    std::vector<const bt::Dataset*> ptrs;
    for (const bt::Dataset& ds : datasets) ptrs.push_back(&ds);
    const std::vector<bt::Params> grid = bt::default_grid();
    const std::size_t tasks = datasets.size() * grid.size();

    // ① 单次回测：JS harness（导出时量的）对比 C++ 引擎（同一份数据、同一组参数，取中位数）
    std::cout << "## One backtest: JS harness vs C++ engine\n\n"
              << "Same data, JS config; C++ time excludes CSV loading; median of " << reps << " runs\n\n"
              << "| channel | orders | candles | JS (s) | C++ (ms) | JS / C++ |\n"
              << "|---|---:|---:|---:|---:|---:|\n";
    for (const bt::Dataset& ds : datasets) {
        std::size_t candles = 0;
        for (const auto& [sym, series] : ds.candles) candles += series.rows.size();
        std::vector<double> secs;
        for (int r = 0; r < reps; ++r) {
            const auto start = std::chrono::steady_clock::now();
            const bt::Result res = bt::run_backtest(ds, bt::Params{});
            secs.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            if (res.final_equity != bt::run_backtest(ds, bt::Params{}).final_equity) return 1;   // 防止被优化掉，也顺便查确定性
        }
        const double cpp = median(secs);
        std::cout << "| " << ds.name << " | " << ds.orders.size() << " | " << candles << " | " << std::fixed
                  << std::setprecision(1) << ds.js_seconds << " | " << std::setprecision(2) << cpp * 1e3 << " | ";
        if (ds.js_seconds > 0) std::cout << std::setprecision(0) << ds.js_seconds / cpp << "x";
        else std::cout << "TBD";
        std::cout << " |\n";
    }
    std::cout << '\n';

    // ② 参数扫描的扩展性
    std::vector<bt::SweepResult> serial;
    std::vector<double> serial_secs;
    for (int r = 0; r < reps; ++r) serial_secs.push_back(seconds_of(ptrs, grid, 0, &serial));
    const double base = median(serial_secs);

    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    std::vector<int> counts = {1, 2, 4};
    if (cores > 4) counts.push_back(cores);
    counts.push_back(2 * cores);

    std::ofstream csv(out_dir + "/sweep_scaling.csv");
    csv << "workers,rep,seconds\n";
    for (int r = 0; r < reps; ++r) csv << 0 << ',' << r << ',' << serial_secs[static_cast<std::size_t>(r)] << '\n';

    std::cout << "## Parameter sweep scaling (WorkStealingPool)\n\n"
              << tasks << " backtests = " << datasets.size() << " channels x " << grid.size()
              << " parameter sets; logical CPUs = " << cores << "; median of " << reps << " runs\n\n"
              << "| workers | time (s) | speedup vs serial | results identical to serial |\n"
              << "|---:|---:|---:|:---:|\n"
              << "| serial | " << std::fixed << std::setprecision(3) << base << " | 1.00x | - |\n";
    bool all_same = true;
    for (int w : counts) {
        std::vector<double> secs;
        bool same = true;
        for (int r = 0; r < reps; ++r) {
            std::vector<bt::SweepResult> res;
            secs.push_back(seconds_of(ptrs, grid, w, &res));
            same = same && same_results(res, serial);
            csv << w << ',' << r << ',' << secs.back() << '\n';
        }
        all_same = all_same && same;
        const double m = median(secs);
        std::cout << "| " << w << " | " << std::setprecision(3) << m << " | " << std::setprecision(2) << base / m
                  << "x | " << (same ? "yes" : "NO") << " |\n";
    }
    std::cout << '\n';
    return all_same ? 0 : 1;
}
