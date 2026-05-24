// locator/locator_engine.h
//
// 聚合 API/String/Pattern 三个扫描器；可选把高分 hit 写入 RAG。
#pragma once

#include "locator/heuristic_hit.h"

#include <functional>
#include <string>
#include <vector>

namespace x64ai {

class LocatorEngine {
public:
    struct Options {
        bool runApi      = true;
        bool runString   = true;
        bool runPattern  = true;
        int  ragMinScore = 60;   // ≥此分数自动写 RAG；0 关闭
        // 用户输入的关键字（原始 token，未归一化）。三个 scanner 各自按自己语义使用。
        std::vector<std::string> userKeywords;
    };

    // 进度回调：phase 形如 "API"/"String"/"Pattern"/"RAG"；done==true 表示该阶段完成
    using ProgressFn =
        std::function<void(const std::string& phase, int current, int total, bool done)>;

    static std::vector<HeuristicHit> runAll(const Options& opt = {},
                                            const ProgressFn& progress = {});
};

}  // namespace x64ai
