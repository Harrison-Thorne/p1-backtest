// 在仓库自带的脱敏样例（data/sample，来自真实频道的导出）上逐笔对照 JS 的结果。
// 换了机器、改了引擎，只要这条测试是绿的，就说明 C++ 撮合层和 JS 仍然一位不差
#include "bt/golden.h"

#include <gtest/gtest.h>

#include <string>

// 两份样例：channel_a 信号简单（只有开单、止盈止损）；channel_b 有大量博主消息触发的改单事件
class GoldenSample : public ::testing::TestWithParam<const char*> {};

TEST_P(GoldenSample, MatchesJsTradeByTrade) {
    const bt::Dataset ds = bt::load_dataset(std::string(SAMPLE_DIR) + "/" + GetParam());
    ASSERT_FALSE(ds.orders.empty());
    ASSERT_FALSE(ds.golden.empty());
    const bt::GoldenReport rep = bt::compare_with_js(ds);
    for (const std::string& m : rep.mismatches) ADD_FAILURE() << m;
    EXPECT_EQ(rep.matched_openings, rep.js_openings);
    EXPECT_EQ(rep.matched_trades, rep.js_trades);
    EXPECT_EQ(rep.cpp_trades, rep.js_trades);
}

INSTANTIATE_TEST_SUITE_P(Samples, GoldenSample, ::testing::Values("channel_a", "channel_b"));

// 反向验证：故意改错一条规则（保本缓冲 0.1% → 0），对照必须失败 —— 证明这个测试真能抓到错
TEST(Golden, DetectsADeliberatelyWrongRule) {
    const bt::Dataset ds = bt::load_dataset(std::string(SAMPLE_DIR) + "/channel_a");
    bt::Params wrong;
    wrong.breakeven_buffer = 0.0;
    const bt::Result good = bt::run_backtest(ds, bt::Params{});
    const bt::Result bad = bt::run_backtest(ds, wrong);
    EXPECT_NE(good.final_equity, bad.final_equity);
}
