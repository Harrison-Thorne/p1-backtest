// 回测输入数据：从 tools/export_from_bot.js 导出的 CSV 目录读进来。
// 读完之后整个 Dataset 只读，多个回测任务可以同时引用同一份，不需要加锁。
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bt {

struct Candle {
    std::int64_t ts;   // 这根 K 线的开始时间（毫秒）
    double open, high, low, close;
};

// 一个币种的全部 K 线，按时间升序
struct CandleSeries {
    std::vector<Candle> rows;
    // 时间恰好等于 ts 的那根；没有（停牌、缺数据）返回 nullptr
    const Candle* at(std::int64_t ts) const;
};

enum class Side { Long, Short };

// JS 版下单那一刻的全部参数。C++ 引擎按同样的公式重新算仓位，换参数时才能得到新的仓位
struct OrderIntent {
    std::int64_t seq;   // 全局序号（和事件共用一个计数），决定同一时刻多件事的先后
    std::int64_t t_ms;
    int id;             // 沿用 JS 数据库里的订单号，事件靠它找到订单
    std::string symbol;
    Side side;
    bool is_limit;
    double entry_price;   // 仓位计算基准价（市价单 = 下单时的价格，限价单 = 入场区间均价）
    std::optional<double> limit_price;
    double stop_loss;
    int leverage;
    double margin;        // JS 实际用的保证金（对照用）
    double size;          // JS 实际下的数量（对照用）
    double sl_distance_pct;
    std::vector<double> tps;          // 止盈价，最多 8 档
    std::vector<double> tp_ratios;    // 每档平掉原始仓位的百分比；空 = 按档数取默认比例
    double available;     // 下单时的可用余额（对照用）
    double position_pct;  // 每单保证金占可用余额的百分比
    bool forced_leverage; // DCA 单沿用主单杠杆，不按止损距离重算
    double size_multiplier;  // 合约每张的数量
    int volume_place;        // 数量保留几位小数
    int max_lever;
    bool contract_known;     // 交易所查不到这个合约时 JS 用默认规格下单，但之后不会检查止盈
};

// 决策层（博主消息触发）的改单事件，回测时原样重放
struct Event {
    std::string kind;     // close / trade / updateOrderRemainingSize / ...
    std::int64_t seq;
    std::int64_t t_ms;
    int order_id;
    std::string a, b, c;  // 参数，含义随 kind 不同
};

// JS 撮合层产生的一笔平仓：C++ 引擎的对照答案
struct GoldenTrade {
    std::int64_t seq;
    std::int64_t t_ms;
    int order_id;
    std::string reason;
    double close_price;
    double close_size;
};

// 频道级开关：决定止损 / 止盈走哪条规则（和 JS 的 channels.js 同名）
struct ChannelFlags {
    bool trail_after_tp1 = false;
    bool tp_ladder_stop = false;
    int tp_ladder_lag = 3;
    bool manual_tp1_tp2 = false;
    bool belovy_mode = false;
    bool entry_mid_to_sl = false;
    bool cmp_to_image_limit = false;
};

struct Dataset {
    std::string name;
    std::vector<OrderIntent> orders;   // 按 seq 升序
    std::vector<Event> events;         // 按 seq 升序
    std::vector<GoldenTrade> golden;   // 按 seq 升序
    std::map<std::string, CandleSeries> candles;
    ChannelFlags flags;
    double initial_balance = 0;
    double js_final_equity = 0;
    std::int64_t gran_ms = 60'000;
    std::int64_t last_message_ms = 0;
    std::int64_t frozen_now_ms = 0;
    double js_seconds = 0;   // JS harness 跑这一次回测用了多久（导出时量的；0 = 没量）
};

// 读一个导出目录；文件缺失或格式不对时抛 std::runtime_error（带文件名和行号）
Dataset load_dataset(const std::string& dir);

}  // namespace bt
