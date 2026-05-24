// locator/heuristic_hit.h
//
// 启发式扫描器统一返回的命中条目。
// 三类扫描器（API/String/Pattern）都把结果转成 HeuristicHit。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x64ai {

enum class HitKind {
    Api,        // 命中导入函数
    String,     // 命中字符串
    Pattern,    // 命中特征码 / 常量
};

inline const char* hitKindName(HitKind k) {
    switch (k) {
        case HitKind::Api:     return "api";
        case HitKind::String:  return "string";
        case HitKind::Pattern: return "pattern";
    }
    return "unknown";
}

struct HeuristicHit {
    HitKind     kind     = HitKind::Api;
    uint64_t    va       = 0;        // 关注的虚拟地址（API：IAT slot 或 xref 调用点；String：字符串内存地址；Pattern：匹配地址）
    uint64_t    refVa    = 0;        // 关联的引用地址（API 的调用点 / String 的引用代码地址）；0 表示无
    std::string label;               // 短标签（API 名 / 字符串前 60 字 / 特征码名）
    std::string category;            // 子类目（如 "anti-debug" / "crypto" / "ascii" / "md5-const"）
    std::string evidence;            // 证据片段（反汇编 1-3 行 / 字符串原文 / 匹配字节）
    int         score    = 0;        // 0-100，越高越值得 AI 关注
};

}  // namespace x64ai
