// locator/api_scanner.h
//
// 扫描主模块 IAT，把"敏感"导入函数标记为命中。
// - 用 Script::Module::GetImports 拿到 IAT
// - 内置敏感 API 分类表（anti-debug / crypto / inject / network / fs-reg / process）
// - 每个命中再用 RefFind / xref 探查调用点（可选，cap 限制）
#pragma once

#include "locator/heuristic_hit.h"

#include <string>
#include <vector>

namespace x64ai {

class ApiScanner {
public:
    // 扫描当前主模块。失败或未在调试返回空。
    // maxXrefsPerApi: 每个敏感 API 最多列出多少个调用点；0=不查 xref
    // userKeywords: 用户关键字（已小写）。IAT 名（归一化后）若包含任一关键字，
    //               即便不在内置敏感表中也会被收录，category="user-keyword"，score=70。
    static std::vector<HeuristicHit> scan(int maxXrefsPerApi = 6,
                                          const std::vector<std::string>& userKeywords = {});

    // 给定 API 名（不区分大小写、忽略 W/A/Ex 后缀），返回匹配的 (category, score)；
    // 未命中返回 {"", 0}
    static std::pair<std::string, int> classify(const std::string& apiName);
};

}  // namespace x64ai
