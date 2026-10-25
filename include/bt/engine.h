// 回测引擎：把 JS bot 的「撮合层」（限价成交 / 止损 / 分批止盈 / 保本）逐行移植过来。
// 「决策层」（AI 解析、频道特例、风控）留在 JS，通过导出的订单和事件流原样输入。
//
// 一次 run() 只读 Dataset、只写自己的局部状态：不同参数的回测可以放进线程池同时跑，互不加锁。
#pragma once
#include "bt/dataset.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bt {

// 分批止盈时每档平掉多少：Js = 沿用 JS 当时的比例；其余三种对任意档数都适用
enum class TpProfile { Js, Front, Even, Back };

// 可以扫描的参数。默认值 = JS 实盘配置，用默认值跑出来的结果必须和 JS 逐笔一致（golden_check）
struct Params {
    std::optional<double> position_pct;       // 覆盖每单仓位百分比；空 = 用 JS 当时的值
    std::optional<int> max_leverage;          // 覆盖杠杆上限；空 = 用 JS 当时算出的杠杆
    TpProfile tp_profile = TpProfile::Js;     // 止盈分配：沿用 JS 的比例，或换成前重 / 均分 / 后重
    double breakeven_buffer = 0.001;          // TP1 后止损移到「入场价 ± 0.1%」
    std::optional<bool> trail_after_tp1;      // 覆盖频道开关：TP1 后改用移动止损
    std::string label = "base";
};

// 一笔平仓记录（分批止盈的每一批各算一笔）
struct Trade {
    std::int64_t t_ms;
    int order_id;
    std::string reason;
    double close_price;
    double close_size;
    double pnl;
};

// 一笔开单时的仓位计算结果：和 JS 当时记下的余额 / 保证金 / 数量对照
struct Opening {
    int order_id;
    double available;
    double margin;
    double size;
};

struct Result {
    std::string label;
    double initial = 0;
    double final_equity = 0;
    double max_drawdown_pct = 0;   // 按每根 K 线时的已实现净值算
    int closes = 0;                // 平仓笔数（和 JS 报告的口径一致）
    int wins = 0;
    int losses = 0;
    std::vector<Trade> trades;
    std::vector<Trade> engine_trades;   // 只含撮合层产生的平仓：和 golden.csv 对照用
    std::vector<Opening> openings;
};

Result run_backtest(const Dataset& ds, const Params& params);

// JS 的 Number.prototype.toFixed 再 parseFloat：按十进制位数四舍五入。
// 仓位、剩余数量都要经过它，C++ 不照做的话会和 JS 差出最后一位
double round_fixed(double x, int digits);

}  // namespace bt
