// 撮合规则的单元测试：每个测试手工搭一个只有一两张单、几根 K 线的小数据集，
// 断言引擎的行为和 JS 版一致（规则出处见 engine.cpp 里各函数上方的注释）
#include "bt/engine.h"

#include <gtest/gtest.h>

#include <array>

namespace {

constexpr std::int64_t kMin = 60'000;
constexpr std::int64_t kT0 = 1'000 * kMin;   // 随便取一个对齐到分钟的起点

// 一张默认的多单：入场 100，止损 90（止损距离 10% → 10 倍杠杆），两档止盈 110 / 120
bt::OrderIntent long_order(int id, std::int64_t t_ms) {
    bt::OrderIntent o{};
    o.seq = id;
    o.t_ms = t_ms;
    o.id = id;
    o.symbol = "AAA";
    o.side = bt::Side::Long;
    o.is_limit = false;
    o.entry_price = 100;
    o.stop_loss = 90;
    o.leverage = 10;
    o.sl_distance_pct = 10;
    o.tps = {110, 120};
    o.tp_ratios = {60, 40};
    o.position_pct = 10;        // 余额 1000 → 保证金 100 → 名义 1000 → 10 个
    o.size = 10;
    o.size_multiplier = 0.01;
    o.volume_place = 2;
    o.max_lever = 50;
    o.contract_known = true;
    return o;
}

// 一个只有 AAA 一个币种的数据集；candles[i] 是第 i 分钟（从 kT0 开始）的 K 线 {open, high, low, close}
bt::Dataset make_dataset(std::vector<bt::OrderIntent> orders, const std::vector<std::array<double, 4>>& candles) {
    bt::Dataset ds;
    ds.name = "unit";
    ds.initial_balance = 1000;
    ds.gran_ms = kMin;
    ds.orders = std::move(orders);
    ds.last_message_ms = ds.orders.empty() ? kT0 : ds.orders.back().t_ms;
    ds.frozen_now_ms = kT0 + static_cast<std::int64_t>(candles.size() + 1) * kMin;
    for (std::size_t i = 0; i < candles.size(); ++i) {
        const auto& c = candles[i];
        ds.candles["AAA"].rows.push_back({kT0 + static_cast<std::int64_t>(i) * kMin, c[0], c[1], c[2], c[3]});
    }
    return ds;
}

std::vector<std::string> reasons(const bt::Result& r) {
    std::vector<std::string> out;
    for (const bt::Trade& t : r.trades) out.push_back(t.reason);
    return out;
}

}  // namespace

TEST(RoundFixed, MatchesJsToFixed) {
    EXPECT_EQ(bt::round_fixed(0.125, 2), 0.13);   // JS (0.125).toFixed(2) === "0.13"，printf 会给 0.12
    EXPECT_EQ(bt::round_fixed(-0.125, 2), -0.13);
    EXPECT_EQ(bt::round_fixed(52.310500000000005, 3), 52.311);
    EXPECT_EQ(bt::round_fixed(9.9996, 3), 10.0);
    EXPECT_EQ(bt::round_fixed(7.0, 0), 7.0);
}

TEST(Engine, SizesPositionFromAvailableBalance) {
    const bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 101, 99, 100}, {100, 101, 99, 100}});
    const bt::Result r = bt::run_backtest(ds, {});
    ASSERT_EQ(r.openings.size(), 1u);
    EXPECT_DOUBLE_EQ(r.openings[0].available, 1000);
    EXPECT_DOUBLE_EQ(r.openings[0].margin, 100);
    EXPECT_DOUBLE_EQ(r.openings[0].size, 10);
}

TEST(Engine, SkipsOrderSmallerThanOneLot) {
    bt::OrderIntent o = long_order(1, kT0);
    o.size_multiplier = 100;   // 名义 1000 / 价格 100 = 10 个，不够一张（100 个）
    const bt::Result r = bt::run_backtest(make_dataset({o}, {{100, 101, 99, 100}}), {});
    EXPECT_TRUE(r.openings.empty());
    EXPECT_DOUBLE_EQ(r.final_equity, 1000);
}

// 消息在 K 线 t 之内到达 → 先推进这根 K 线、再开单，所以新单从下一根才开始检查（避免前视）
TEST(Engine, NewOrderIsNotCheckedAgainstItsOwnCandle) {
    const bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 130, 80, 100}, {100, 101, 99, 100}});
    const bt::Result r = bt::run_backtest(ds, {});
    EXPECT_EQ(reasons(r), (std::vector<std::string>{"backtest_end_mtm"}));
}

// 同一根 K 线既碰到止损又碰到止盈：保守口径，先判止损
TEST(Engine, StopLossWinsWhenCandleTouchesBoth) {
    const bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 100, 100, 100}, {100, 125, 85, 100}});
    const bt::Result r = bt::run_backtest(ds, {});
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].reason, "stop_loss");
    EXPECT_DOUBLE_EQ(r.trades[0].close_price, 90);   // 止损按止损价成交
    EXPECT_DOUBLE_EQ(r.final_equity, 1000 - 100);    // (90-100) × 10
}

// TP1：按比例部分平仓（成交价取 K 线最高价），止损移到保本；之后跌回保本价全平剩余
TEST(Engine, Tp1PartialCloseThenBreakeven) {
    const bt::Dataset ds = make_dataset({long_order(1, kT0)},
                                        {{100, 100, 100, 100}, {100, 112, 101, 111}, {111, 111, 100, 100}});
    const bt::Result r = bt::run_backtest(ds, {});
    ASSERT_EQ(reasons(r), (std::vector<std::string>{"tp1", "breakeven_sl"}));
    EXPECT_DOUBLE_EQ(r.trades[0].close_size, 6);      // 10 × 60%
    EXPECT_DOUBLE_EQ(r.trades[0].close_price, 112);
    EXPECT_DOUBLE_EQ(r.trades[1].close_size, 4);
    EXPECT_DOUBLE_EQ(r.trades[1].close_price, 100 * 1.001);
}

// 一根 K 线冲过所有止盈：同一根里连续推进，最后一档全平
TEST(Engine, OneCandleCanWalkThroughAllTps) {
    const bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 100, 100, 100}, {100, 125, 101, 124}});
    const bt::Result r = bt::run_backtest(ds, {});
    EXPECT_EQ(reasons(r), (std::vector<std::string>{"tp1", "tp2"}));
}

TEST(Engine, ShortSideIsMirrored) {
    bt::OrderIntent o = long_order(1, kT0);
    o.side = bt::Side::Short;
    o.stop_loss = 110;
    o.tps = {90, 80};
    const bt::Dataset ds = make_dataset({o}, {{100, 100, 100, 100}, {100, 99, 88, 89}, {89, 101, 89, 100}});
    const bt::Result r = bt::run_backtest(ds, {});
    ASSERT_EQ(reasons(r), (std::vector<std::string>{"tp1", "breakeven_sl"}));
    EXPECT_DOUBLE_EQ(r.trades[0].close_price, 88);
    EXPECT_DOUBLE_EQ(r.trades[0].pnl, (100 - 88) * 6.0);
    EXPECT_DOUBLE_EQ(r.trades[1].close_price, 100 * 0.999);
}

TEST(Engine, LimitOrderFillsAtLimitPriceOnlyWhenTouched) {
    bt::OrderIntent o = long_order(1, kT0);
    o.is_limit = true;
    o.limit_price = 95;
    const bt::Dataset ds = make_dataset({o}, {{100, 100, 100, 100}, {100, 101, 96, 99}, {99, 99, 94, 96},
                                              {96, 96, 89, 90}});
    const bt::Result r = bt::run_backtest(ds, {});
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].reason, "stop_loss");
    EXPECT_DOUBLE_EQ(r.trades[0].pnl, (90 - 95) * 10.0);   // 入场价 = 限价 95
}

// 没成交的挂单在回测结束时撤掉：不产生盈亏，占用的保证金还回来
TEST(Engine, UnfilledLimitIsCancelledAtEnd) {
    bt::OrderIntent o = long_order(1, kT0);
    o.is_limit = true;
    o.limit_price = 50;
    const bt::Result r = bt::run_backtest(make_dataset({o}, {{100, 100, 100, 100}, {100, 101, 99, 100}}), {});
    EXPECT_TRUE(r.trades.empty());
    EXPECT_DOUBLE_EQ(r.final_equity, 1000);
}

// 已有同向主仓时，挂单成交 = 加仓：数量并进主仓，入场价按数量加权
TEST(Engine, FilledLimitMergesIntoPrimaryAsDca) {
    bt::OrderIntent dca = long_order(2, kT0 + kMin);
    dca.is_limit = true;
    dca.limit_price = 95;
    dca.entry_price = 95;
    const bt::Dataset ds = make_dataset({long_order(1, kT0), dca},
                                        {{100, 100, 100, 100}, {100, 100, 100, 100}, {100, 100, 94, 95},
                                         {95, 95, 85, 86}});
    const bt::Result r = bt::run_backtest(ds, {});
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].order_id, 1);
    EXPECT_EQ(r.trades[0].reason, "stop_loss");
    // 加仓单开单时可用余额 900 → 保证金 90 → 名义 900 → 按 95 算 9.47 个
    EXPECT_DOUBLE_EQ(r.trades[0].close_size, 10 + 9.47);
    const double avg = bt::round_fixed((100 * 10 + 95 * 9.47) / 19.47, 6);
    EXPECT_DOUBLE_EQ(r.trades[0].pnl, bt::round_fixed((90 - avg) * 19.47, 4));
}

TEST(Engine, Tp1CancelsPendingLimitsOnSameSide) {
    bt::OrderIntent dca = long_order(2, kT0);
    dca.is_limit = true;
    dca.limit_price = 80;
    const bt::Dataset ds = make_dataset({long_order(1, kT0), dca},
                                        {{100, 100, 100, 100}, {100, 112, 101, 111}, {111, 111, 70, 75}});
    const bt::Result r = bt::run_backtest(ds, {});
    // TP1 后挂单被撤：价格跌到 70 也不会再加仓，只有主仓的保本止损
    EXPECT_EQ(reasons(r), (std::vector<std::string>{"tp1", "breakeven_sl"}));
}

// 交易所查不到合约规格：JS 照样下单，但止盈检查会失败跳过，只剩止损
TEST(Engine, UnknownContractNeverTakesProfit) {
    bt::OrderIntent o = long_order(1, kT0);
    o.contract_known = false;
    const bt::Dataset ds = make_dataset({o}, {{100, 100, 100, 100}, {100, 125, 101, 124}, {124, 124, 89, 90}});
    const bt::Result r = bt::run_backtest(ds, {});
    EXPECT_EQ(reasons(r), (std::vector<std::string>{"stop_loss"}));
}

TEST(Engine, CloseEventFromDecisionLayerIsReplayed) {
    bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 100, 100, 100}, {100, 105, 99, 104},
                                                          {104, 104, 89, 90}});
    // 第 1 分钟博主喊离场：JS 记下一笔按 104 平 10 个，并关闭订单
    ds.events.push_back({"trade", 10, kT0 + kMin, 1, "exit_by_blogger", "104", "10"});
    ds.events.push_back({"close", 11, kT0 + kMin, 1, "exit_by_blogger", "", ""});
    ds.last_message_ms = kT0 + kMin;
    const bt::Result r = bt::run_backtest(ds, {});
    EXPECT_EQ(reasons(r), (std::vector<std::string>{"exit_by_blogger"}));
    EXPECT_DOUBLE_EQ(r.final_equity, 1000 + 40);
}

TEST(Engine, PositionPctParameterScalesSizeAndEventSizes) {
    bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 100, 100, 100}, {100, 105, 99, 104}});
    ds.events.push_back({"trade", 10, kT0 + kMin, 1, "exit_by_blogger", "104", "10"});
    ds.events.push_back({"close", 11, kT0 + kMin, 1, "exit_by_blogger", "", ""});
    bt::Params p;
    p.position_pct = 20;   // 仓位翻倍 → 博主喊的「平 10 个」按比例变成平 20 个
    const bt::Result r = bt::run_backtest(ds, p);
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_DOUBLE_EQ(r.trades[0].close_size, 20);
}

TEST(Engine, MaxLeverageParameterRecomputesLeverage) {
    bt::Params p;
    p.max_leverage = 5;   // 止损距离 10% 本来算出 10 倍，上限压到 5 倍 → 仓位减半
    const bt::Result r = bt::run_backtest(make_dataset({long_order(1, kT0)}, {{100, 100, 100, 100}}), p);
    ASSERT_EQ(r.openings.size(), 1u);
    EXPECT_DOUBLE_EQ(r.openings[0].size, 5);
}

TEST(Engine, TrailingStopFollowsHighAfterTp1) {
    bt::Params p;
    p.trail_after_tp1 = true;
    // TP1 在 112 成交后最高到 115：移动止损 = 115 × (1 - 10%) = 103.5，高于保本价，所以按 103.5 出
    const bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 100, 100, 100}, {100, 112, 104, 111},
                                                               {111, 115, 104, 114}, {114, 114, 103, 103}});
    const bt::Result r = bt::run_backtest(ds, p);
    ASSERT_EQ(reasons(r), (std::vector<std::string>{"tp1", "trailing_sl"}));
    EXPECT_DOUBLE_EQ(r.trades[1].close_price, 115 * (1 - 0.1));
}

TEST(Engine, ReportsMaxDrawdown) {
    const bt::Dataset ds = make_dataset({long_order(1, kT0)}, {{100, 100, 100, 100}, {100, 100, 89, 90}});
    const bt::Result r = bt::run_backtest(ds, {});
    EXPECT_DOUBLE_EQ(r.max_drawdown_pct, 10.0);   // 1000 → 900
}
