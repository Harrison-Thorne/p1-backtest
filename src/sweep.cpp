#include "bt/sweep.h"

#include "conc/work_stealing_pool.h"

#include <future>
#include <sstream>
#include <string>

namespace bt {

std::vector<Params> default_grid() {
    const std::vector<double> position_pcts = {2, 3, 4, 5, 6};
    const std::vector<std::optional<int>> max_levs = {std::nullopt, 10, 20, 50};
    const std::vector<std::pair<std::string, TpProfile>> ratio_sets = {
        {"js", TpProfile::Js},
        {"front", TpProfile::Front},   // 前几档多平：落袋为安
        {"even", TpProfile::Even},
        {"back", TpProfile::Back},     // 留更多仓位等后面几档
    };
    const std::vector<bool> trails = {false, true};
    const std::vector<double> be_buffers = {0.0, 0.001, 0.003};

    std::vector<Params> grid;
    for (double pct : position_pcts) {
        for (const auto& lev : max_levs) {
            for (const auto& [ratio_name, profile] : ratio_sets) {
                for (bool trail : trails) {
                    for (double be : be_buffers) {
                        Params p;
                        p.position_pct = pct;
                        p.max_leverage = lev;
                        p.tp_profile = profile;
                        p.trail_after_tp1 = trail;
                        p.breakeven_buffer = be;
                        std::ostringstream label;
                        label << "pct=" << pct << " lev=" << (lev ? std::to_string(*lev) : "js")
                              << " tp=" << ratio_name << " trail=" << trail << " be=" << be;
                        p.label = label.str();
                        grid.push_back(std::move(p));
                    }
                }
            }
        }
    }
    return grid;
}

namespace {

Result run_one(const Dataset& ds, const Params& p) {
    Result r = run_backtest(ds, p);
    r.trades.clear();
    r.trades.shrink_to_fit();
    r.engine_trades.clear();
    r.engine_trades.shrink_to_fit();
    r.openings.clear();
    r.openings.shrink_to_fit();
    return r;
}

}  // namespace

std::vector<SweepResult> run_sweep(const std::vector<const Dataset*>& datasets,
                                   const std::vector<Params>& grid, int workers) {
    std::vector<SweepResult> out;
    out.reserve(datasets.size() * grid.size());
    for (const Dataset* ds : datasets) {
        for (const Params& p : grid) out.push_back({ds, p, Result{}});
    }

    if (workers <= 0) {
        for (SweepResult& r : out) r.result = run_one(*r.dataset, r.params);
        return out;
    }

    // 每个任务按值捕获指针：out 在所有 future 取完之前不会扩容，指针一直有效。
    // 结果通过 future 带回来按下标放好，所以输出顺序和哪个线程先做完无关。
    // pool 必须声明在 out 之后：某个 get() 抛异常时局部变量逆序析构，
    // 先析构 pool（等还没跑完的任务做完），再析构它们正在读的 out
    WorkStealingPool pool(workers);
    std::vector<std::future<Result>> futures;
    futures.reserve(out.size());
    for (const SweepResult& r : out) {
        const Dataset* ds = r.dataset;
        const Params* p = &r.params;
        futures.push_back(pool.submit([ds, p] { return run_one(*ds, *p); }));
    }
    for (std::size_t i = 0; i < out.size(); ++i) out[i].result = futures[i].get();   // 任务抛的异常在这里重新抛出
    return out;
}

}  // namespace bt
