# p1-backtest

C++17 port of the order-matching core of a JS crypto trading bot's backtester.
The JS harness's decisions (parsed signals, channel rules, risk checks) are exported as an
event stream; this engine replays limit fills, stop-loss, partial take-profit and break-even
on 1-minute candles. Standard library + GoogleTest only.

Work in progress.
