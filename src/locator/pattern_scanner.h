// locator/pattern_scanner.h
//
// 在主模块代码/数据节里扫描已知"指纹"：
//   - 加密常量（MD5 init、SHA1 init、SHA256 K、AES Sbox 头、CRC32 表头）
//   - 加壳/保护壳头标志（UPX!, VMProtect, Themida, .enigma1）
//   - 简单字节级特征码（短模式，禁用过短以免误报）
#pragma once

#include "locator/heuristic_hit.h"

#include <string>
#include <vector>

namespace x64ai {

class PatternScanner {
public:
    // userKeywords: 用户关键字。
    //   - 形如 "DE AD BE EF" / "48 8B ?? E8"（空格可选；?? / ? 通配整字节）→ 按字节模式扫描
    //   - 否则按 ASCII 字符串（区分大小写）扫描；以及尝试 UTF-16LE 扫描
    // 命中 category="user-keyword"，score=75
    static std::vector<HeuristicHit> scan(const std::vector<std::string>& userKeywords = {});
};

}  // namespace x64ai
