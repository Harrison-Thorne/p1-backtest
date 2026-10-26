# Benchmark summary

- date: 2026-10-26T06:42Z
- CPU: 12th Gen Intel(R) Core(TM) i7-12700H (20 logical CPUs)
- compiler: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- kernel: 6.6.123.2-microsoft-standard-WSL2+
- power (/sys/class/power_supply/AC1/online): 1

## Golden check (C++ vs JS, trade by trade)

Only channels marked [OK] are used below.

```
[OK]   ch01: openings 32/32, trades 18/18 (cpp 18), equity js=928.642 cpp=928.642
[OK]   ch02: openings 92/92, trades 95/95 (cpp 95), equity js=769.005 cpp=769.005
[OK]   ch03: openings 157/157, trades 116/116 (cpp 116), equity js=837.63 cpp=837.63
[OK]   ch04: openings 58/58, trades 56/56 (cpp 56), equity js=1131.04 cpp=1131.04
[OK]   ch05: openings 74/74, trades 41/41 (cpp 41), equity js=1286.56 cpp=1286.56
[ERROR] data/exports/ch06: ch06: belovyMode channels are not supported by the C++ engine
[OK]   ch07: openings 50/50, trades 100/100 (cpp 100), equity js=826.056 cpp=826.056
[OK]   ch08: openings 96/96, trades 168/168 (cpp 168), equity js=1179.02 cpp=1179.02
[OK]   ch09: openings 111/111, trades 170/170 (cpp 170), equity js=1078.48 cpp=1078.48
[OK]   ch10: openings 183/183, trades 100/100 (cpp 100), equity js=786.693 cpp=786.693
[OK]   ch11: openings 25/25, trades 22/22 (cpp 22), equity js=1175.73 cpp=1175.73
```

## One backtest: JS harness vs C++ engine

Same data, JS config; C++ time excludes CSV loading; median of 5 runs

| channel | orders | candles | JS (s) | C++ (ms) | JS / C++ |
|---|---:|---:|---:|---:|---:|
| ch01 | 32 | 142668 | 50.0 | 22.61 | 2212x |
| ch02 | 92 | 7585275 | 267.3 | 128.33 | 2083x |
| ch03 | 157 | 5560738 | 277.7 | 88.16 | 3150x |
| ch04 | 58 | 6045964 | 100.3 | 35.70 | 2810x |
| ch05 | 74 | 4311382 | 111.8 | 50.55 | 2212x |
| ch07 | 50 | 2588228 | 76.4 | 9.04 | 8449x |
| ch08 | 96 | 10800381 | 371.8 | 239.39 | 1553x |
| ch09 | 111 | 6015426 | 643.0 | 442.50 | 1453x |
| ch10 | 183 | 13549072 | 368.7 | 150.97 | 2442x |
| ch11 | 25 | 400181 | 34.9 | 17.15 | 2035x |

## Parameter sweep scaling (WorkStealingPool)

4800 backtests = 10 channels x 480 parameter sets; logical CPUs = 20; median of 5 runs

| workers | time (s) | speedup vs serial | results identical to serial |
|---:|---:|---:|:---:|
| serial | 540.493 | 1.00x | - |
| 1 | 535.595 | 1.01x | yes |
| 2 | 271.232 | 1.99x | yes |
| 4 | 135.182 | 4.00x | yes |
| 20 | 52.697 | 10.26x | yes |
| 40 | 52.566 | 10.28x | yes |

## Sweep results

```
4800 backtests (10 channels x 480 parameter sets) on 20 workers in 55.58 s -> results/sweep.csv

ch01: JS config -> equity 928.64, max DD 9.71%
  #1 pct=2 lev=10 tp=js trail=0 be=0.003 -> equity 990.60, max DD 1.48%
  #2 pct=2 lev=10 tp=front trail=0 be=0.003 -> equity 990.34, max DD 1.47%
  #3 pct=2 lev=10 tp=js trail=0 be=0.001 -> equity 990.16, max DD 1.50%
ch02: JS config -> equity 769.00, max DD 26.79%
  #1 pct=2 lev=50 tp=back trail=1 be=0.003 -> equity 832.97, max DD 21.10%
  #2 pct=2 lev=50 tp=front trail=1 be=0.003 -> equity 832.97, max DD 21.10%
  #3 pct=2 lev=js tp=js trail=1 be=0.003 -> equity 832.97, max DD 21.10%
ch03: JS config -> equity 837.63, max DD 24.57%
  #1 pct=2 lev=10 tp=front trail=1 be=0.003 -> equity 961.77, max DD 12.75%
  #2 pct=2 lev=10 tp=js trail=1 be=0.003 -> equity 959.95, max DD 12.67%
  #3 pct=2 lev=10 tp=front trail=1 be=0.001 -> equity 956.04, max DD 13.06%
ch04: JS config -> equity 1131.04, max DD 9.42%
  #1 pct=6 lev=50 tp=js trail=0 be=0.003 -> equity 1256.33, max DD 17.60%
  #2 pct=6 lev=js tp=js trail=0 be=0.003 -> equity 1256.33, max DD 17.60%
  #3 pct=6 lev=50 tp=back trail=0 be=0.003 -> equity 1247.01, max DD 17.60%
ch05: JS config -> equity 1286.56, max DD 17.23%
  #1 pct=6 lev=js tp=back trail=0 be=0.003 -> equity 1426.48, max DD 23.49%
  #2 pct=6 lev=50 tp=back trail=0 be=0.003 -> equity 1426.48, max DD 23.49%
  #3 pct=6 lev=20 tp=back trail=0 be=0.003 -> equity 1424.30, max DD 20.84%
ch07: JS config -> equity 826.06, max DD 20.75%
  #1 pct=2 lev=20 tp=front trail=0 be=0 -> equity 905.18, max DD 12.07%
  #2 pct=2 lev=js tp=front trail=0 be=0 -> equity 905.18, max DD 12.07%
  #3 pct=2 lev=50 tp=front trail=0 be=0 -> equity 905.18, max DD 12.07%
ch08: JS config -> equity 1179.02, max DD 10.00%
  #1 pct=6 lev=50 tp=front trail=1 be=0.001 -> equity 1409.76, max DD 18.93%
  #2 pct=6 lev=js tp=front trail=1 be=0.001 -> equity 1409.76, max DD 18.93%
  #3 pct=6 lev=50 tp=front trail=1 be=0 -> equity 1408.30, max DD 18.97%
ch09: JS config -> equity 1078.48, max DD 14.95%
  #1 pct=3 lev=20 tp=back trail=1 be=0.001 -> equity 1138.53, max DD 11.91%
  #2 pct=3 lev=20 tp=back trail=0 be=0.001 -> equity 1138.53, max DD 11.91%
  #3 pct=4 lev=20 tp=back trail=1 be=0.001 -> equity 1136.91, max DD 14.09%
ch10: JS config -> equity 786.69, max DD 21.90%
  #1 pct=2 lev=10 tp=back trail=0 be=0 -> equity 947.79, max DD 6.97%
  #2 pct=2 lev=10 tp=back trail=0 be=0.003 -> equity 946.91, max DD 7.50%
  #3 pct=2 lev=10 tp=even trail=0 be=0 -> equity 944.00, max DD 7.21%
ch11: JS config -> equity 1175.73, max DD 4.33%
  #1 pct=6 lev=50 tp=back trail=1 be=0.003 -> equity 1335.78, max DD 5.32%
  #2 pct=6 lev=js tp=back trail=1 be=0.003 -> equity 1335.78, max DD 5.32%
  #3 pct=6 lev=20 tp=back trail=1 be=0.003 -> equity 1331.77, max DD 5.32%
```

whole run: about 2 h 15 min (from the summary.md timestamps, 14:43 to 16:58 local; the timing line was fixed in the script after this run)
