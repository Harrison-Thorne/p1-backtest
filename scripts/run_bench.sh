#!/usr/bin/env bash
# 一键压测：Release 构建 → 逐笔对照 JS → 单次回测 JS vs C++ → 参数扫描扩展性 → 扫描结果。
# 输出写到 results/（原始 CSV + summary.md）。压测前请插上电源。
# 用法：bash scripts/run_bench.sh [导出目录的上级，默认 data/exports]
set -euo pipefail
cd "$(dirname "$0")/.."

EXPORTS=${1:-data/exports}
mkdir -p results
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3" > /dev/null
cmake --build build-release -j "$(nproc)" > /dev/null

ALL_DIRS=$(find "$EXPORTS" -mindepth 1 -maxdepth 1 -type d | sort)
# 只有逐笔对上 JS 的频道才进入压测和扫描：没对上的频道，扫出来的数字没有意义
DIRS=""
for d in $ALL_DIRS; do
    if ./build-release/golden_check "$d" > /dev/null; then DIRS="$DIRS $d"; fi
done
SUMMARY=results/summary.md
START=$(date +%s)
{
    echo "# Benchmark summary"
    echo
    echo "- date: $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "- CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs) ($(nproc) logical CPUs)"
    echo "- compiler: $(c++ --version | head -1)"
    echo "- kernel: $(uname -r)"
    for s in /sys/class/power_supply/*/online; do [ -f "$s" ] && echo "- power ($s): $(cat "$s")"; done
    echo
    echo "## Golden check (C++ vs JS, trade by trade)"
    echo
    echo "Only channels marked [OK] are used below."
    echo
    echo '```'
    ./build-release/golden_check $ALL_DIRS || true
    echo '```'
    echo
    ./build-release/bench_sweep --reps 5 --out results $DIRS
    echo "## Sweep results"
    echo
    echo '```'
    ./build-release/sweep --out results/sweep.csv $DIRS
    echo '```'
    echo
    echo "whole run: $(( $(date +%s) - START )) s"
} | tee "$SUMMARY"
