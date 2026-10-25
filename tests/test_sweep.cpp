// 参数扫描的并发正确性：并行跑出来的每个结果都必须和顺序跑的完全相同，且顺序固定
#include "bt/sweep.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

std::vector<bt::Params> small_grid() {
    std::vector<bt::Params> grid = bt::default_grid();
    grid.resize(24);   // 测试只取前 24 组，够覆盖多个 worker 同时跑
    return grid;
}

}  // namespace

TEST(Sweep, DefaultGridHasExpectedSize) {
    EXPECT_EQ(bt::default_grid().size(), 5u * 4u * 4u * 2u * 3u);
}

TEST(Sweep, ParallelResultsEqualSerialResultsInSameOrder) {
    const bt::Dataset ds = bt::load_dataset(std::string(SAMPLE_DIR) + "/channel_b");
    const std::vector<const bt::Dataset*> datasets = {&ds};
    const std::vector<bt::Params> grid = small_grid();

    const auto serial = bt::run_sweep(datasets, grid, 0);
    const auto parallel = bt::run_sweep(datasets, grid, 4);
    ASSERT_EQ(serial.size(), grid.size());
    ASSERT_EQ(parallel.size(), grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
        EXPECT_EQ(parallel[i].params.label, grid[i].label);
        EXPECT_EQ(parallel[i].result.final_equity, serial[i].result.final_equity) << grid[i].label;
        EXPECT_EQ(parallel[i].result.closes, serial[i].result.closes) << grid[i].label;
        EXPECT_EQ(parallel[i].result.max_drawdown_pct, serial[i].result.max_drawdown_pct) << grid[i].label;
    }
}

TEST(Sweep, DifferentParametersGiveDifferentResults) {
    const bt::Dataset ds = bt::load_dataset(std::string(SAMPLE_DIR) + "/channel_b");
    const auto results = bt::run_sweep({&ds}, small_grid(), 2);
    bool any_different = false;
    for (const auto& r : results) any_different = any_different || r.result.final_equity != results[0].result.final_equity;
    EXPECT_TRUE(any_different);   // 否则说明参数根本没传进引擎
}

// 某个任务抛异常（不支持的频道类型）：异常经 future 传回调用线程，不会悄悄丢掉或让程序崩溃
TEST(Sweep, TaskExceptionReachesCaller) {
    bt::Dataset ds = bt::load_dataset(std::string(SAMPLE_DIR) + "/channel_b");
    ds.flags.belovy_mode = true;
    EXPECT_THROW(bt::run_sweep({&ds}, small_grid(), 4), std::runtime_error);
}
