// 撮合层移植。每个函数旁边标了它对应的 JS 函数（bot 仓库 src/position/monitor.js 等），
// 逐行对照着读。和 JS 不一样的地方只有一类：JS 把订单存在 SQLite 里，这里存在内存的 vector 里。
#include "bt/engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace bt {

double round_fixed(double x, int digits) {
    // JS 的 toFixed 按「十进制下最近，一样近取绝对值大的」舍入，而 printf 遇到恰好一半时取偶数。
    // 先多打 30 位拿到这个 double 的精确十进制展开，再自己做「逢 5 进位」，两边就完全一致
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", digits + 30, std::fabs(x));
    std::string s(buf);
    const std::size_t dot = s.find('.');
    std::string kept = s.substr(0, dot) + s.substr(dot + 1, static_cast<std::size_t>(digits));
    const bool round_up = s[dot + 1 + static_cast<std::size_t>(digits)] >= '5';
    if (round_up) {
        int i = static_cast<int>(kept.size()) - 1;
        while (i >= 0 && kept[static_cast<std::size_t>(i)] == '9') kept[static_cast<std::size_t>(i--)] = '0';
        if (i < 0) kept.insert(kept.begin(), '1');
        else ++kept[static_cast<std::size_t>(i)];
    }
    if (digits > 0) kept.insert(kept.size() - static_cast<std::size_t>(digits), ".");
    const double v = std::strtod(kept.c_str(), nullptr);
    return x < 0 ? -v : v;
}

namespace {

constexpr int kMaxTp = 8;

enum class OrderType { Limit, Market, DcaMerged };

// 对应 JS 数据库 orders 表的一行（只保留撮合层用得到的列）
struct Order {
    int id;
    std::string symbol;
    Side side;
    OrderType type;
    double entry_price;
    std::optional<double> limit_price;
    double stop_loss;
    int leverage;
    double margin_used;
    double original_size;
    double remaining_size;
    double sl_distance_pct;
    std::array<std::optional<double>, kMaxTp> tp{};
    std::array<bool, kMaxTp> tp_hit{};
    std::vector<double> tp_ratios;   // 空 = 用默认比例
    bool breakeven_set = false;
    std::optional<double> trail_high;
    std::optional<double> tp_base_size;
    bool closed = false;
    std::int64_t closed_by_event_seq = -1;   // 被哪条决策层 close 事件关掉的（-1 = 不是）
    // 合约规格
    double size_multiplier;
    int volume_place;
    bool contract_known;
    // C++ 仓位 / JS 仓位：换了仓位参数后，决策层事件里的「平多少」要按这个比例换算
    double size_scale = 1.0;

    bool is_long() const { return side == Side::Long; }
};

// JS monitor.js defaultRatios
std::vector<double> default_ratios(std::size_t n) {
    if (n == 1) return {100};
    if (n == 2) return {60, 40};
    if (n == 3) return {40, 40, 20};
    if (n == 4) return {30, 30, 20, 20};
    if (n == 5) return {25, 25, 20, 20, 10};
    return std::vector<double>(n, round_fixed(100.0 / static_cast<double>(n), 4));
}

// 扫描用的止盈分配：n 档里第 i 档的权重，再归一化成百分比（最后一档总是全平剩余，权重只影响前 n-1 档）
std::vector<double> profile_ratios(TpProfile profile, std::size_t n) {
    std::vector<double> w(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (profile == TpProfile::Front) w[i] = static_cast<double>(n - i);   // n, n-1, …, 1
        if (profile == TpProfile::Even) w[i] = 1;
        if (profile == TpProfile::Back) w[i] = static_cast<double>(i + 1);    // 1, 2, …, n
    }
    double total = 0;
    for (double x : w) total += x;
    for (double& x : w) x = round_fixed(x / total * 100.0, 4);
    return w;
}

std::string tp_reason(std::size_t i) { return "tp" + std::to_string(i + 1); }

class Backtester {
public:
    Backtester(const Dataset& ds, const Params& p) : ds_(ds), p_(p) {
        equity_ = ds.initial_balance;
        peak_ = equity_;
        trail_after_tp1_ = p.trail_after_tp1.value_or(ds.flags.trail_after_tp1);
    }

    Result run();

private:
    // ---- 账户（JS database.js 的 paper_account）----
    double available() const {
        double used = 0;
        for (const Order& o : orders_) {
            if (!o.closed) used += o.margin_used;
        }
        return equity_ - used;
    }

    // JS database.js saveTradeLog：按价格差算盈亏，计入净值
    void save_trade_log(const Order& o, double close_price, double close_size, const std::string& reason,
                        bool from_engine) {
        if (close_size <= 0) return;
        const double dir = o.is_long() ? 1.0 : -1.0;
        const double pnl = round_fixed(dir * (close_price - o.entry_price) * close_size, 4);
        equity_ += pnl;
        const Trade t{now_, o.id, reason, close_price, close_size, pnl};
        result_.trades.push_back(t);
        if (from_engine) result_.engine_trades.push_back(t);
        if (reason != "dca_merged" && reason != "manual_cancel") {
            ++result_.closes;
            if (pnl > 0) ++result_.wins;
            if (pnl < 0) ++result_.losses;
        }
    }

    Order* find(int id) {
        auto it = index_.find(id);
        return it == index_.end() ? nullptr : &orders_[it->second];
    }

    // ---- 下单（JS order.js executeOrder + calcLeverage + calcSize）----
    void open_order(const OrderIntent& in);

    // ---- 撮合（JS monitor.js）----
    void replay_symbol_candle(const std::string& symbol, const Candle& c);
    bool try_fill_limit(Order& o, double fill_probe);
    void check_position(Order& o, double sl_probe, double tp_probe);
    void check_tp(Order& o, double mark);
    void perform_exit(Order& o, const std::string& reason, double close_price);
    void cancel_pending_limits(const Order& filled, const std::string& reason);

    // ---- 决策层事件重放 ----
    void apply_event(const Event& e);

    void finish(std::int64_t end_ms);

    const Dataset& ds_;
    const Params& p_;
    bool trail_after_tp1_;
    std::vector<Order> orders_;     // 按开单顺序（= JS 的 id 顺序）
    std::map<int, std::size_t> index_;
    double equity_;
    double peak_;
    std::int64_t now_ = 0;
    bool in_engine_ = false;
    Result result_;
};

void Backtester::open_order(const OrderIntent& in) {
    // 杠杆：默认沿用 JS 算出的值；扫描杠杆上限时按 calcLeverage 的公式重算（DCA 单沿用主单杠杆，不重算）
    int leverage = in.leverage;
    if (p_.max_leverage && !in.forced_leverage) {
        const int max_lev = in.contract_known ? std::min(*p_.max_leverage, in.max_lever) : *p_.max_leverage;
        const int raw = static_cast<int>(std::floor(1.0 / (in.sl_distance_pct / 100.0)));
        leverage = std::max(1, std::min(max_lev, raw));
    }
    const double balance = available();
    const double pct = p_.position_pct.value_or(in.position_pct);
    const double margin = balance * pct / 100.0;
    const double notional = margin * leverage;
    const double lots = std::floor(notional / in.entry_price / in.size_multiplier);
    const double size = round_fixed(lots * in.size_multiplier, in.volume_place);
    if (balance < margin || lots < 1) return;   // JS：余额不足 / 不足 1 张 → 不开单
    result_.openings.push_back({in.id, balance, margin, size});

    Order o{};
    o.id = in.id;
    o.symbol = in.symbol;
    o.side = in.side;
    o.type = in.is_limit ? OrderType::Limit : OrderType::Market;
    o.entry_price = in.entry_price;
    o.limit_price = in.limit_price;
    o.stop_loss = in.stop_loss;
    o.leverage = leverage;
    o.margin_used = margin;
    o.original_size = size;
    o.remaining_size = size;
    o.sl_distance_pct = in.sl_distance_pct;
    for (std::size_t i = 0; i < in.tps.size() && i < kMaxTp; ++i) o.tp[i] = in.tps[i];
    o.tp_ratios = in.tp_ratios;
    if (p_.tp_profile != TpProfile::Js && in.tps.size() >= 2) o.tp_ratios = profile_ratios(p_.tp_profile, in.tps.size());
    o.size_multiplier = in.size_multiplier;
    o.volume_place = in.volume_place;
    o.contract_known = in.contract_known;
    o.size_scale = in.size > 0 ? size / in.size : 1.0;
    index_[o.id] = orders_.size();
    orders_.push_back(std::move(o));
}

// JS replaySymbolCandle：先处理限价成交，再按「先止损后止盈」推进持仓
void Backtester::replay_symbol_candle(const std::string& symbol, const Candle& c) {
    in_engine_ = true;
    // 先把这一刻的挂单列表定下来（JS 是一次 SQL 查询），之后成交的单不会重复处理
    std::vector<int> limits;
    for (const Order& o : orders_) {
        if (!o.closed && o.symbol == symbol && o.type == OrderType::Limit) limits.push_back(o.id);
    }
    for (int id : limits) {
        Order& lo = *find(id);
        if (lo.closed) continue;
        if ((ds_.flags.entry_mid_to_sl || ds_.flags.cmp_to_image_limit) && lo.tp[0]) {
            const bool reached_tp1 = lo.is_long() ? c.high >= *lo.tp[0] : c.low <= *lo.tp[0];
            if (reached_tp1) {
                lo.closed = true;   // skipped_ran_up：价格没等到回调就冲到了 TP1，撤单不入场
                continue;
            }
        }
        try_fill_limit(lo, lo.is_long() ? c.low : c.high);
    }

    std::vector<int> live;
    for (const Order& o : orders_) {
        if (!o.closed && o.symbol == symbol && o.type == OrderType::Market) live.push_back(o.id);
    }
    for (int id : live) {
        Order& o = *find(id);
        const double adverse = o.is_long() ? c.low : c.high;
        const double favorable = o.is_long() ? c.high : c.low;
        // check_tp 每次只推进一档，同一根 K 线反复推进，直到平仓或不再有进展（JS 同样上限 6 次，只比较前 5 档）
        for (int guard = 0; guard < 6; ++guard) {
            if (o.closed) break;
            const std::array<bool, kMaxTp> before = o.tp_hit;
            check_position(o, adverse, favorable);
            if (o.closed) break;
            if (std::equal(before.begin(), before.begin() + 5, o.tp_hit.begin())) break;
        }
    }
    in_engine_ = false;
}

// JS _paperTryFillLimit：触及限价 → 有同向主仓就并进去（DCA），没有就升级为主仓
bool Backtester::try_fill_limit(Order& o, double fill_probe) {
    const double limit = o.limit_price.value_or(o.entry_price);
    const bool filled = o.is_long() ? fill_probe <= limit : fill_probe >= limit;
    if (!filled) return false;

    Order* primary = nullptr;
    for (Order& other : orders_) {
        if (!other.closed && other.id != o.id && other.symbol == o.symbol && other.side == o.side &&
            other.type == OrderType::Market) {
            primary = &other;
            break;
        }
    }
    if (primary) {
        const double dca_size = o.original_size;
        const double primary_size = primary->remaining_size;
        primary->original_size += dca_size;
        primary->remaining_size += dca_size;
        if (limit > 0 && primary->entry_price > 0) {
            const double total = primary_size + dca_size;
            primary->entry_price = round_fixed((primary->entry_price * primary_size + limit * dca_size) / total, 6);
        }
        o.closed = true;   // dca_merged：这张挂单的保证金随之释放（JS 同样如此）
        o.type = OrderType::DcaMerged;
    } else {
        o.type = OrderType::Market;
        o.entry_price = limit;
    }
    return true;
}

// JS _paperCheckPosition：算出当前有效止损价，先判止损；没止损再交给 check_tp
void Backtester::check_position(Order& o, double sl_probe, double tp_probe) {
    const bool is_long = o.is_long();
    const double be_price = is_long ? o.entry_price * (1 + p_.breakeven_buffer)
                                    : o.entry_price * (1 - p_.breakeven_buffer);
    double eff_sl = o.stop_loss;
    std::string reason_after_tp1 = "breakeven_sl";

    if (ds_.flags.tp_ladder_stop) {
        // 阶梯止损：按已命中的档数推进，永远不比保本价差
        int hit_count = 0;
        for (bool h : o.tp_hit) hit_count += h ? 1 : 0;
        if (hit_count >= 1) {
            double ladder = be_price;
            const int idx = hit_count - ds_.flags.tp_ladder_lag;
            const bool use_tp = idx >= 1 && o.tp[static_cast<std::size_t>(idx - 1)] &&
                                *o.tp[static_cast<std::size_t>(idx - 1)] != 0;
            if (use_tp) ladder = *o.tp[static_cast<std::size_t>(idx - 1)];
            eff_sl = is_long ? std::max(be_price, ladder) : std::min(be_price, ladder);
            reason_after_tp1 = use_tp ? "ladder_sl" : "breakeven_sl";
        }
    } else if (o.breakeven_set) {
        if (trail_after_tp1_) {
            // 移动止损：跟着最有利的价格走，保持开仓时的止损距离，同样不比保本价差
            const double dist = o.sl_distance_pct / 100.0;
            const double high = is_long ? std::max(o.trail_high.value_or(o.entry_price), tp_probe)
                                        : std::min(o.trail_high.value_or(o.entry_price), tp_probe);
            o.trail_high = high;
            const double trail_sl = is_long ? high * (1 - dist) : high * (1 + dist);
            eff_sl = is_long ? std::max(be_price, trail_sl) : std::min(be_price, trail_sl);
            reason_after_tp1 = "trailing_sl";
        } else {
            eff_sl = be_price;
        }
    }

    const bool sl_hit = eff_sl > 0 && (is_long ? sl_probe <= eff_sl : sl_probe >= eff_sl);
    if (sl_hit) {
        const bool moved = o.breakeven_set || reason_after_tp1 == "ladder_sl";
        perform_exit(o, moved ? reason_after_tp1 : "stop_loss", eff_sl);   // 止损按止损价成交
        return;
    }
    check_tp(o, tp_probe);
}

// JS checkTp（模拟盘分支）：按顺序推进一档止盈；中间档部分平仓，最后一档全平
void Backtester::check_tp(Order& o, double mark) {
    if (!o.contract_known) return;   // JS：getContractInfo 失败 → 跳过本次止盈检查
    const double orig_size = o.tp_base_size.value_or(o.original_size);
    auto lot_floor = [&o](double size) {
        return round_fixed(std::floor(size / o.size_multiplier) * o.size_multiplier, o.volume_place);
    };

    std::vector<std::size_t> stages;   // 有价格的止盈档的下标
    for (std::size_t i = 0; i < kMaxTp; ++i) {
        if (o.tp[i]) stages.push_back(i);
    }
    std::vector<double> ratios = o.tp_ratios;
    if (ratios.size() < stages.size()) ratios = default_ratios(stages.size());
    const bool is_long = o.is_long();
    auto reached = [&](std::size_t k) { return is_long ? mark >= *o.tp[k] : mark <= *o.tp[k]; };

    if (ds_.flags.manual_tp1_tp2 && !stages.empty()) {
        // 中间档由博主消息驱动（作为事件重放），这里只看最后一档
        const std::size_t last = stages.back();
        if (!o.tp_hit[last] && reached(last)) perform_exit(o, tp_reason(last), mark);
        return;
    }

    for (std::size_t s = 0; s < stages.size(); ++s) {
        const std::size_t k = stages[s];
        if (o.tp_hit[k]) continue;
        if (s > 0 && !o.tp_hit[stages[s - 1]]) break;
        if (!reached(k)) break;

        if (s == stages.size() - 1) {   // 最后一档：全部离场，按 K 线有利极值成交
            perform_exit(o, tp_reason(k), mark);
            return;
        }
        // 运算顺序必须和 JS 一样：先 ratio = 40 / 100，再乘数量。
        // 0.43 × 40 / 100 和 0.43 × (40 / 100) 的最后一位不同，向下取整到整张后就会差一张
        const double ratio = ratios[s] / 100.0;
        const double close_size = lot_floor(orig_size * ratio);
        const double remaining = round_fixed(o.remaining_size - close_size, o.volume_place);
        o.tp_hit[k] = true;
        o.remaining_size = remaining;
        if (s == 0) o.breakeven_set = true;   // TP1 后止损移到保本
        save_trade_log(o, mark, close_size, tp_reason(k), in_engine_);
        if (s == 0) cancel_pending_limits(o, "tp1_cancel");
        return;   // 每次只推进一档
    }

    if (stages.empty()) {   // 没有止盈价：浮盈达到 300% 离场
        const double profit_pct = (is_long ? (mark - o.entry_price) : (o.entry_price - mark)) /
                                  o.entry_price * o.leverage * 100.0;
        if (profit_pct >= 300) perform_exit(o, "profit_150pct", mark);
    }
}

// JS performExit：全平剩余仓位
void Backtester::perform_exit(Order& o, const std::string& reason, double close_price) {
    o.closed = true;
    save_trade_log(o, close_price, o.remaining_size, reason, in_engine_);
}

// JS cancelPendingLimits：TP1 后撤掉同币种同方向还没成交的挂单（不再加仓）
void Backtester::cancel_pending_limits(const Order& filled, const std::string& /*reason*/) {
    for (Order& o : orders_) {
        if (!o.closed && o.type == OrderType::Limit && o.symbol == filled.symbol && o.side == filled.side) {
            o.closed = true;
        }
    }
}

// 决策层事件：每种 kind 对应 JS database.js 里同名的一个函数，这里做同样的修改。
// 订单在这组参数下没开出来（比如仓位太小不足 1 张）或已经平掉时，事件直接忽略
void Backtester::apply_event(const Event& e) {
    Order* o = find(e.order_id);
    if (!o) return;
    auto args = [&e]() {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : e.a) {
            if (ch == '|') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        out.push_back(cur);
        return out;
    }();
    auto num = [](const std::string& s) { return std::stod(s); };
    auto scaled = [o](double js_size) {
        return round_fixed(js_size * o->size_scale, o->volume_place);
    };

    if (e.kind == "close") {
        if (!o->closed) o->closed_by_event_seq = e.seq;
        o->closed = true;
    } else if (e.kind == "trade") {
        // 博主喊离场 / 部分止盈：按 JS 当时的成交价，平掉按比例换算后的数量。
        // 没有成交价（JS 平的是没成交的挂单）时 JS 不计盈亏，也不算进平仓笔数
        if (e.b.empty()) return;
        // JS 的 performExit 先关订单、紧接着记这笔平仓，所以「刚被上一条事件关掉」的单照样记。
        // 换了参数后这张单可能早被止损平掉了，那时就不能再平一次
        const bool just_closed = o->closed_by_event_seq == e.seq - 1;
        if (o->closed && !just_closed) return;
        save_trade_log(*o, num(e.b), scaled(num(e.c)), e.a, false);
    } else if (e.kind == "updateOrderRemainingSize") {
        o->remaining_size = scaled(num(args.at(0)));
    } else if (e.kind.rfind("updateOrderTp", 0) == 0 && e.kind.size() == 17 &&
               e.kind.compare(14, 3, "Hit") == 0) {
        const std::size_t k = static_cast<std::size_t>(e.kind[13] - '1');   // updateOrderTp<N>Hit
        o->tp_hit[k] = true;
        o->remaining_size = scaled(num(args.at(0)));
    } else if (e.kind == "updateOrderBreakeven") {
        o->breakeven_set = true;
    } else if (e.kind == "updateOrderEntryPrice") {
        o->entry_price = num(args.at(0));
    } else if (e.kind == "promoteOrderToMarket") {
        o->type = OrderType::Market;
    } else if (e.kind == "absorbDcaIntoOrder") {
        o->original_size += scaled(num(args.at(0)));
        o->remaining_size += scaled(num(args.at(0)));
    } else if (e.kind == "setOrderTpBaseSize") {
        o->tp_base_size = scaled(num(args.at(0)));
    } else if (e.kind == "updateOrderTp1Price") {
        o->tp[0] = num(args.at(0));
    } else if (e.kind == "updateOrderTp2Price") {
        o->tp[1] = num(args.at(0));
    } else if (e.kind == "updateOrderTakeProfits") {
        // 参数：tp1..tp5, tpJson, forcedRatios
        for (std::size_t i = 0; i < 5; ++i) {
            const std::string& s = args.at(i);
            o->tp[i] = (s == "null" || s.empty()) ? std::nullopt : std::optional<double>(num(s));
        }
        if (args.size() > 6 && args[6] != "null" && !args[6].empty()) {
            std::vector<double> r;
            std::string body = args[6].substr(1, args[6].size() - 2);   // 去掉 [ ]
            std::size_t start = 0;
            while (start < body.size()) {
                std::size_t end = body.find(';', start);
                if (end == std::string::npos) end = body.size();
                r.push_back(num(body.substr(start, end - start)));
                start = end + 1;
            }
            o->tp_ratios = r;
        }
    } else {
        throw std::runtime_error("unsupported event kind: " + e.kind);
    }
}

// JS run.js 收尾：还开着的仓位按最后一根 K 线收盘价平掉，没成交的挂单撤掉
void Backtester::finish(std::int64_t end_ms) {
    now_ = end_ms;
    in_engine_ = true;
    const std::int64_t grid = end_ms / ds_.gran_ms * ds_.gran_ms;
    for (Order& o : orders_) {
        if (o.closed || o.type == OrderType::Limit) continue;
        double px = o.entry_price;
        auto it = ds_.candles.find(o.symbol);
        if (it != ds_.candles.end()) {
            const Candle* c = it->second.at(grid);
            if (!c) c = it->second.at(grid - ds_.gran_ms);
            if (c) px = c->close;
        }
        perform_exit(o, "backtest_end_mtm", px);
    }
    for (Order& o : orders_) o.closed = true;
    in_engine_ = false;
}

Result Backtester::run() {
    result_.label = p_.label;
    result_.initial = ds_.initial_balance;
    if (ds_.orders.empty()) {
        result_.final_equity = equity_;
        return result_;
    }
    const std::int64_t gran = ds_.gran_ms;
    // 和 JS 一样：最多回放到最后一条消息之后 60 天
    const std::int64_t end_ms = std::min(ds_.frozen_now_ms - gran, ds_.last_message_ms + std::int64_t{60} * 24 * 3600 * 1000);
    const std::int64_t start = ds_.orders.front().t_ms / gran * gran;

    std::size_t next_order = 0;
    std::size_t next_event = 0;
    for (std::int64_t gt = start; gt <= end_ms; gt += gran) {
        now_ = gt;
        // 1) 用这根 K 线推进所有有未平订单的币种（按订单顺序去重，和 JS 一致）
        std::vector<std::string> symbols;
        for (const Order& o : orders_) {
            if (!o.closed && std::find(symbols.begin(), symbols.end(), o.symbol) == symbols.end()) {
                symbols.push_back(o.symbol);
            }
        }
        for (const std::string& sym : symbols) {
            auto it = ds_.candles.find(sym);
            if (it == ds_.candles.end()) continue;
            if (const Candle* c = it->second.at(gt)) replay_symbol_candle(sym, *c);
        }
        // 2) 再处理时间落在这根 K 线之前的消息：开单和决策层事件按全局序号交错
        while (true) {
            const bool has_order = next_order < ds_.orders.size() && ds_.orders[next_order].t_ms <= gt;
            const bool has_event = next_event < ds_.events.size() && ds_.events[next_event].t_ms <= gt;
            if (!has_order && !has_event) break;
            const bool order_first = has_order && (!has_event || ds_.orders[next_order].seq < ds_.events[next_event].seq);
            if (order_first) {
                now_ = ds_.orders[next_order].t_ms;
                open_order(ds_.orders[next_order++]);
            } else {
                now_ = ds_.events[next_event].t_ms;
                apply_event(ds_.events[next_event++]);
            }
        }
        peak_ = std::max(peak_, equity_);
        if (peak_ > 0) result_.max_drawdown_pct = std::max(result_.max_drawdown_pct, (peak_ - equity_) / peak_ * 100.0);

        const bool any_open = std::any_of(orders_.begin(), orders_.end(), [](const Order& o) { return !o.closed; });
        if (gt >= ds_.last_message_ms && !any_open) break;   // 消息处理完且没有持仓，提前结束
    }
    finish(end_ms);
    result_.final_equity = equity_;
    return result_;
}

}  // namespace

Result run_backtest(const Dataset& ds, const Params& params) {
    if (ds.flags.belovy_mode) {
        throw std::runtime_error(ds.name + ": belovyMode channels are not supported by the C++ engine");
    }
    Backtester bt(ds, params);
    return bt.run();
}

}  // namespace bt
