// 对照检查：用默认参数（= JS 当时的配置）跑 C++ 引擎，和 JS 的结果逐笔比较。
// 这是「C++ 移植没写错」的证据，测试和 golden_check 程序共用这一份比较逻辑
#pragma once
#include "bt/dataset.h"
#include "bt/engine.h"

#include <string>
#include <vector>

namespace bt {

struct GoldenReport {
    std::size_t js_trades = 0;
    std::size_t cpp_trades = 0;
    std::size_t matched_trades = 0;
    std::size_t js_openings = 0;
    std::size_t matched_openings = 0;
    double js_equity = 0;
    double cpp_equity = 0;
    std::vector<std::string> mismatches;   // 前若干条不一致的描述，空 = 完全一致

    bool ok() const { return mismatches.empty(); }
};

GoldenReport compare_with_js(const Dataset& ds, std::size_t max_mismatches = 10);

}  // namespace bt
