#!/usr/bin/env bash
# 两遍正确性检查：TSan（参数扫描的线程池部分）→ ASan+UBSan。任何一步失败立即以非 0 退出。
# 用法：bash scripts/check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS=$(nproc)
# 新内核的地址随机化范围更大，TSan 会报 unexpected memory mapping；
# setarch -R 只对这条命令关闭随机化，不影响系统
NO_ASLR="setarch $(uname -m) -R"

step() { printf '\n========== %s ==========\n' "$*"; }

step "1/2 ThreadSanitizer (data races)"
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON > /dev/null
# 构建时 gtest_discover_tests 要运行测试程序列出用例，所以构建也要关随机化
$NO_ASLR cmake --build build-tsan -j "$JOBS"
TSAN_OPTIONS="halt_on_error=1" $NO_ASLR ctest --test-dir build-tsan --output-on-failure

step "2/2 AddressSanitizer + UndefinedBehaviorSanitizer (memory errors, UB, leaks)"
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON > /dev/null
cmake --build build-asan -j "$JOBS"
ASAN_OPTIONS="detect_leaks=1" ctest --test-dir build-asan --output-on-failure

step "ALL CHECKS PASSED"
