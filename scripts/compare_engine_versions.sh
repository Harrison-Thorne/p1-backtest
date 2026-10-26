#!/usr/bin/env bash
# 优化前后对比：分别构建 M3（优化前的引擎）和 M4（优化后）两个提交，
# 用同一批数据各跑 3 次完整参数扫描，打印每次的用时。两边的扫描结果必须逐字节相同。
# 用法：bash scripts/compare_engine_versions.sh <导出目录>...
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 1 ] || { echo "usage: $0 <dataset dir>..."; exit 2; }

WORK=$(mktemp -d)
trap 'git worktree remove --force "$WORK/before" 2>/dev/null; git worktree remove --force "$WORK/after" 2>/dev/null; rm -rf "$WORK"' EXIT

build() {   # $1 = 提交信息前缀，$2 = 目录名
    local commit
    commit=$(git log --format=%h --grep="^$1:" | head -1)
    git worktree add --quiet "$WORK/$2" "$commit"
    git -C "$WORK/$2" submodule update --init --quiet
    cmake -S "$WORK/$2" -B "$WORK/$2/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3" > /dev/null
    cmake --build "$WORK/$2/build" -j "$(nproc)" --target sweep > /dev/null
    echo "$2 = $1 ($commit)"
}
build M3 before
build M4 after

for v in before after; do
    for r in 1 2 3; do
        "$WORK/$v/build/sweep" --out "$WORK/$v.csv" "$@" | head -1 | sed "s/^/$v run $r: /"
    done
done
cmp "$WORK/before.csv" "$WORK/after.csv" && echo "sweep results identical"
