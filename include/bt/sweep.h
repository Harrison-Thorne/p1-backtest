// 参数扫描：「每个频道 × 每组参数」是一个独立的回测任务，交给 p0 的 WorkStealingPool 并行跑。
// 任务之间只共享只读的 Dataset，各自的账户、订单状态都在任务内部，所以不需要任何锁。
#pragma once
#include "bt/dataset.h"
#include "bt/engine.h"

#include <vector>

namespace bt {

struct SweepResult {
    const Dataset* dataset;
    Params params;
    Result result;   // 为了省内存，扫描时不保留逐笔成交（trades 为空）
};

// 默认扫描网格：仓位 × 杠杆上限 × 止盈分配 × 移动止损 × 保本缓冲
std::vector<Params> default_grid();

// workers <= 0 表示不开线程池，在调用线程里顺序跑（对照组）。
// 返回顺序固定为「数据集在外、参数在内」，和线程调度无关 —— 并行和顺序的结果可以逐个比较
std::vector<SweepResult> run_sweep(const std::vector<const Dataset*>& datasets,
                                   const std::vector<Params>& grid, int workers);

}  // namespace bt
