#include "bt/golden.h"

#include <cmath>
#include <sstream>

namespace bt {

namespace {

// 两边做的是同样的浮点运算，理论上应当一位不差；留一个极小的相对误差，
// 只是为了不让 CSV 里十进制转二进制的最后一位把检查搞红
bool same(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

}  // namespace

GoldenReport compare_with_js(const Dataset& ds, std::size_t max_mismatches) {
    GoldenReport rep;
    const Result r = run_backtest(ds, Params{});
    rep.js_trades = ds.golden.size();
    rep.cpp_trades = r.engine_trades.size();
    rep.js_openings = ds.orders.size();
    rep.js_equity = ds.js_final_equity;
    rep.cpp_equity = r.final_equity;
    auto note = [&rep, max_mismatches](const std::string& s) {
        if (rep.mismatches.size() < max_mismatches) rep.mismatches.push_back(s);
    };

    // 1) 开单：每一笔的可用余额、保证金、数量
    for (std::size_t i = 0; i < ds.orders.size(); ++i) {
        const OrderIntent& js = ds.orders[i];
        if (i >= r.openings.size() || r.openings[i].order_id != js.id) {
            note("order " + std::to_string(js.id) + ": not opened by C++");
            continue;
        }
        const Opening& c = r.openings[i];
        if (same(c.available, js.available) && same(c.margin, js.margin) && same(c.size, js.size)) {
            ++rep.matched_openings;
        } else {
            std::ostringstream s;
            s.precision(12);
            s << "order " << js.id << ": available/margin/size js=" << js.available << "/" << js.margin << "/"
              << js.size << " cpp=" << c.available << "/" << c.margin << "/" << c.size;
            note(s.str());
        }
    }

    // 2) 撮合层的每一笔平仓：顺序、订单号、原因、价格、数量都要一致
    const std::size_t n = std::max(ds.golden.size(), r.engine_trades.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (i >= ds.golden.size() || i >= r.engine_trades.size()) {
            note("trade #" + std::to_string(i) + ": only one side has it");
            continue;
        }
        const GoldenTrade& js = ds.golden[i];
        const Trade& c = r.engine_trades[i];
        if (js.order_id == c.order_id && js.reason == c.reason && js.t_ms == c.t_ms &&
            same(js.close_price, c.close_price) && same(js.close_size, c.close_size)) {
            ++rep.matched_trades;
        } else {
            std::ostringstream s;
            s.precision(12);
            s << "trade #" << i << ": js=(t=" << js.t_ms << " order " << js.order_id << " " << js.reason << " @"
              << js.close_price << " x" << js.close_size << ") cpp=(t=" << c.t_ms << " order " << c.order_id
              << " " << c.reason << " @" << c.close_price << " x" << c.close_size << ")";
            note(s.str());
        }
    }

    // 3) 最终净值
    if (!same(rep.js_equity, rep.cpp_equity)) {
        std::ostringstream s;
        s.precision(12);
        s << "final equity js=" << rep.js_equity << " cpp=" << rep.cpp_equity;
        note(s.str());
    }
    return rep;
}

}  // namespace bt
