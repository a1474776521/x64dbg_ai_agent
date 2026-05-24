// locator/string_scanner.h
//
// 扫描主模块所有可读非代码节，提取 ASCII (≥4) 与 UTF-16LE (≥4) 字符串，
// 按关键词加权，再用 xref 找出引用代码地址。
#pragma once

#include "locator/heuristic_hit.h"

#include <string>
#include <vector>

namespace x64ai {

class StringScanner {
public:
    struct Options {
        int  minAsciiLen   = 5;
        int  minUtf16Len   = 5;
        int  maxStrings    = 4000;   // 内部抽取上限，避免极端样本爆内存
        int  maxXrefsPerStr = 2;     // 每个字符串最多列几个引用点（0=不查 xref）
        bool keepUnscoredWithXref = false;  // 没命中关键词但有引用的也保留（低分）
        // 用户自定义关键字（已小写化）。命中任一关键字 +40 分，
        // category 标记为 "user-keyword"（若内置词表也命中则保留较高分的那个）。
        std::vector<std::string> userKeywords;
    };

    static std::vector<HeuristicHit> scan(const Options& opt = {});
};

}  // namespace x64ai
