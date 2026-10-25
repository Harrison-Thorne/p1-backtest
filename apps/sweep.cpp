// 用法: sweep [--workers N] [--out results/sweep.csv] <导出目录>...
// 对每个频道跑完整个默认参数网格，结果写成 CSV，并打印每个频道收益最高的几组参数
#include "bt/sweep.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    int workers = static_cast<int>(std::thread::hardware_concurrency());
    std::string out_path = "results/sweep.csv";
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--workers" && i + 1 < argc) {
            workers = std::stoi(argv[++i]);
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            dirs.push_back(arg);
        }
    }
    if (dirs.empty()) {
        std::cerr << "usage: sweep [--workers N] [--out file.csv] <dataset dir>...\n";
        return 2;
    }

    std::vector<bt::Dataset> datasets;
    for (const std::string& d : dirs) datasets.push_back(bt::load_dataset(d));
    std::vector<const bt::Dataset*> ptrs;
    for (const bt::Dataset& ds : datasets) ptrs.push_back(&ds);

    const std::vector<bt::Params> grid = bt::default_grid();
    const auto start = std::chrono::steady_clock::now();
    const std::vector<bt::SweepResult> results = bt::run_sweep(ptrs, grid, workers);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::ofstream csv(out_path);
    csv << "channel,params,final_equity,roi_pct,max_drawdown_pct,closes,wins,losses\n";
    for (const bt::SweepResult& r : results) {
        const bt::Result& res = r.result;
        csv << r.dataset->name << ",\"" << r.params.label << "\"," << std::fixed << std::setprecision(4)
            << res.final_equity << ',' << (res.final_equity - res.initial) / res.initial * 100 << ','
            << res.max_drawdown_pct << ',' << res.closes << ',' << res.wins << ',' << res.losses << '\n';
    }

    std::cout << results.size() << " backtests (" << datasets.size() << " channels x " << grid.size()
              << " parameter sets) on " << workers << " workers in " << std::fixed << std::setprecision(2)
              << secs << " s -> " << out_path << "\n\n";

    // 每个频道：JS 原配置的结果 + 收益最高的 3 组
    for (const bt::Dataset& ds : datasets) {
        std::vector<const bt::SweepResult*> mine;
        for (const bt::SweepResult& r : results) {
            if (r.dataset == &ds) mine.push_back(&r);
        }
        std::sort(mine.begin(), mine.end(), [](const auto* a, const auto* b) {
            return a->result.final_equity > b->result.final_equity;
        });
        const bt::Result base = bt::run_backtest(ds, bt::Params{});
        std::cout << ds.name << ": JS config -> equity " << std::setprecision(2) << base.final_equity
                  << ", max DD " << base.max_drawdown_pct << "%\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(3, mine.size()); ++i) {
            const bt::Result& res = mine[i]->result;
            std::cout << "  #" << i + 1 << " " << mine[i]->params.label << " -> equity " << res.final_equity
                      << ", max DD " << res.max_drawdown_pct << "%\n";
        }
    }
    return 0;
}
