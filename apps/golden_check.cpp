// 用法: golden_check [--max 不一致最多打印几条，默认 10] <导出目录>...
// 对每个目录：用 JS 当时的配置跑 C++ 引擎，逐笔对照 JS 的开单和平仓，全部一致才返回 0
#include "bt/golden.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: golden_check <dataset dir>...\n";
        return 2;
    }
    bool all_ok = true;
    std::size_t max_mismatches = 10;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--max" && i + 1 < argc) {
            max_mismatches = std::stoul(argv[++i]);
            continue;
        }
        try {
            const bt::Dataset ds = bt::load_dataset(argv[i]);
            const bt::GoldenReport rep = bt::compare_with_js(ds, max_mismatches);
            std::cout << (rep.ok() ? "[OK]   " : "[FAIL] ") << ds.name << ": openings " << rep.matched_openings
                      << "/" << rep.js_openings << ", trades " << rep.matched_trades << "/" << rep.js_trades
                      << " (cpp " << rep.cpp_trades << "), equity js=" << rep.js_equity
                      << " cpp=" << rep.cpp_equity << "\n";
            for (const std::string& m : rep.mismatches) std::cout << "         " << m << "\n";
            all_ok = all_ok && rep.ok();
        } catch (const std::exception& e) {
            std::cout << "[ERROR] " << argv[i] << ": " << e.what() << "\n";
            all_ok = false;
        }
    }
    return all_ok ? 0 : 1;
}
