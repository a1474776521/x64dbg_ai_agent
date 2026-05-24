// locator/pattern_scanner.cpp
#include "locator/pattern_scanner.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

#include "_plugins.h"
#include "_scriptapi_memory.h"
#include "_scriptapi_module.h"
#include "bridgemain.h"

#include "util/logging.h"

namespace x64ai {

namespace {

struct BytePattern {
    const char*    name;
    const char*    category;
    int            score;
    const uint8_t* bytes;
    size_t         len;
};

// === 加密常量 ===
// MD5 initial hash values (little-endian as appears in code/data):
// 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476
const uint8_t kMd5Init[]   = {
    0x01,0x23,0x45,0x67, 0x89,0xAB,0xCD,0xEF,
    0xFE,0xDC,0xBA,0x98, 0x76,0x54,0x32,0x10
};
// SHA1 init: 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
const uint8_t kSha1Init[]  = {
    0x01,0x23,0x45,0x67, 0x89,0xAB,0xCD,0xEF,
    0xFE,0xDC,0xBA,0x98, 0x76,0x54,0x32,0x10,
    0xF0,0xE1,0xD2,0xC3
};
// SHA256 K[0..3]: 0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5
const uint8_t kSha256K[]   = {
    0x98,0x2F,0x8A,0x42, 0x91,0x44,0x37,0x71,
    0xCF,0xFB,0xC0,0xB5, 0xA5,0xDB,0xB5,0xE9
};
// AES Sbox head: 0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76
const uint8_t kAesSbox[]   = {
    0x63,0x7C,0x77,0x7B, 0xF2,0x6B,0x6F,0xC5,
    0x30,0x01,0x67,0x2B, 0xFE,0xD7,0xAB,0x76
};
// CRC32 (poly 0xEDB88320) table head:
// 0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA
const uint8_t kCrc32Tab[]  = {
    0x00,0x00,0x00,0x00, 0x96,0x30,0x07,0x77,
    0x2C,0x61,0x0E,0xEE, 0xBA,0x51,0x09,0x99
};
// ZIP local file header
const uint8_t kZipHdr[]    = { 'P','K',0x03,0x04 };
// PE 头（找内嵌 PE，可能是 dropper / shellcode）
const uint8_t kPeHdr[]     = { 'M','Z' };  // 太短，单独处理（要求后跟 'PE\0\0' 才报）

// === 加壳壳头标志 ===
const uint8_t kUpx[]       = { 'U','P','X','!' };
const uint8_t kVmProtect[] = { 'V','M','P','r','o','t','e','c','t' };
const uint8_t kThemida[]   = { 'T','h','e','m','i','d','a' };
const uint8_t kEnigma[]    = { '.','e','n','i','g','m','a' };
const uint8_t kAspack[]    = { 'a','s','p','a','c','k','d','i','e' };
const uint8_t kPetite[]    = { 'P','E','t','i','t','e' };

const BytePattern kPatterns[] = {
    { "MD5_init",     "crypto-const",  85, kMd5Init,   sizeof(kMd5Init) },
    { "SHA1_init",    "crypto-const",  85, kSha1Init,  sizeof(kSha1Init) },
    { "SHA256_K",     "crypto-const",  90, kSha256K,   sizeof(kSha256K) },
    { "AES_Sbox",     "crypto-const",  95, kAesSbox,   sizeof(kAesSbox) },
    { "CRC32_table",  "crypto-const",  70, kCrc32Tab,  sizeof(kCrc32Tab) },
    { "ZIP_header",   "embedded-blob", 75, kZipHdr,    sizeof(kZipHdr) },
    { "UPX_signature","packer",        90, kUpx,       sizeof(kUpx) },
    { "VMProtect",    "packer",        95, kVmProtect, sizeof(kVmProtect) },
    { "Themida",      "packer",        95, kThemida,   sizeof(kThemida) },
    { "Enigma",       "packer",        90, kEnigma,    sizeof(kEnigma) },
    { "ASPack",       "packer",        85, kAspack,    sizeof(kAspack) },
    { "PEtite",       "packer",        85, kPetite,    sizeof(kPetite) },
};

// 在 buf 内查找所有 pattern 出现的偏移。简单 memmem 循环（可接受：节大小通常 < 几 MB）
void findAll(const std::vector<uint8_t>& buf, const uint8_t* p, size_t n,
             std::vector<size_t>& out, int maxHits)
{
    if (n == 0 || buf.size() < n) return;
    const size_t end = buf.size() - n;
    for (size_t i = 0; i <= end; ++i) {
        if (buf[i] == p[0] && std::memcmp(&buf[i], p, n) == 0) {
            out.push_back(i);
            if (static_cast<int>(out.size()) >= maxHits) return;
            i += n - 1;  // 跳过已匹配段
        }
    }
}

// 内嵌 PE：必须 'MZ' + e_lfanew 处指向 'PE\0\0'
void findEmbeddedPE(const std::vector<uint8_t>& buf, std::vector<size_t>& out,
                    int maxHits)
{
    for (size_t i = 0; i + 64 < buf.size(); ++i) {
        if (buf[i] != 'M' || buf[i + 1] != 'Z') continue;
        // e_lfanew @ +0x3C (4 bytes LE)
        if (i + 0x40 > buf.size()) continue;
        uint32_t e_lfanew =
            (uint32_t)buf[i + 0x3C]        |
            ((uint32_t)buf[i + 0x3D] << 8) |
            ((uint32_t)buf[i + 0x3E] << 16)|
            ((uint32_t)buf[i + 0x3F] << 24);
        if (e_lfanew < 0x40 || e_lfanew > 0x1000) continue;  // 合理范围
        size_t peOff = i + e_lfanew;
        if (peOff + 4 > buf.size()) continue;
        if (buf[peOff] == 'P' && buf[peOff + 1] == 'E' &&
            buf[peOff + 2] == 0   && buf[peOff + 3] == 0) {
            // 跳过文件头自身（i==0 时是宿主自己）
            if (i == 0) continue;
            out.push_back(i);
            if (static_cast<int>(out.size()) >= maxHits) return;
        }
    }
}

std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return r;
}

// 判断字符串是否"看起来像"十六进制 pattern：
//   含 hex 字符或 ?/??，去掉空白后长度为偶数（每字节 2 个 hex 位）。
//   ?? 或 ? 视为整字节通配。
bool looksLikeHexPattern(const std::string& s)
{
    int nibble = 0;
    bool sawHexOrWild = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '?') { sawHexOrWild = true; nibble++; continue; }
        if ((uc >= '0' && uc <= '9') ||
            (uc >= 'a' && uc <= 'f') ||
            (uc >= 'A' && uc <= 'F')) {
            sawHexOrWild = true;
            nibble++;
            continue;
        }
        return false;
    }
    return sawHexOrWild && (nibble % 2 == 0) && nibble >= 2;
}

// 解析 hex pattern：输出 bytes[] 和 mask[]（mask[i]=true 表示该字节有效，false 表示通配）
// 失败返回 false。
bool parseHexPattern(const std::string& s,
                     std::vector<uint8_t>& bytes,
                     std::vector<bool>& mask)
{
    bytes.clear();
    mask.clear();
    std::string compact;
    compact.reserve(s.size());
    for (char c : s) if (c != ' ' && c != '\t') compact.push_back(c);
    if (compact.size() % 2 != 0 || compact.empty()) return false;

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < compact.size(); i += 2) {
        char a = compact[i], b = compact[i + 1];
        if (a == '?' && b == '?') {
            bytes.push_back(0);
            mask.push_back(false);
        } else if (a == '?' || b == '?') {
            // 半通配：当作整字节通配处理（简化）
            bytes.push_back(0);
            mask.push_back(false);
        } else {
            int hi = hexVal(a), lo = hexVal(b);
            if (hi < 0 || lo < 0) return false;
            bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
            mask.push_back(true);
        }
    }
    // 至少要有一个有效字节，否则全通配毫无意义
    bool anyValid = false;
    for (bool m : mask) if (m) { anyValid = true; break; }
    return anyValid;
}

// 带 mask 的 memmem
void findAllMasked(const std::vector<uint8_t>& buf,
                   const std::vector<uint8_t>& pat,
                   const std::vector<bool>& mask,
                   std::vector<size_t>& out, int maxHits)
{
    size_t n = pat.size();
    if (n == 0 || buf.size() < n) return;
    const size_t end = buf.size() - n;
    for (size_t i = 0; i <= end; ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (mask[j] && buf[i + j] != pat[j]) { ok = false; break; }
        }
        if (ok) {
            out.push_back(i);
            if (static_cast<int>(out.size()) >= maxHits) return;
            i += n - 1;
        }
    }
}

// 把 ASCII 字符串转为 UTF-16LE 字节序列
std::vector<uint8_t> asciiToUtf16LE(const std::string& s)
{
    std::vector<uint8_t> r;
    r.reserve(s.size() * 2);
    for (char c : s) {
        r.push_back(static_cast<uint8_t>(c));
        r.push_back(0);
    }
    return r;
}

}  // namespace

std::vector<HeuristicHit> PatternScanner::scan(const std::vector<std::string>& userKeywords)
{
    std::vector<HeuristicHit> out;

    if (!DbgIsDebugging()) {
        XAI_LOG_WARN("PatternScanner: not debugging");
        return out;
    }

    Script::Module::ModuleInfo mod{};
    if (!Script::Module::GetMainModuleInfo(&mod)) return out;

    ListInfo li{};
    if (!Script::Module::GetMainModuleSectionList(&li) || li.count <= 0 || !li.data) {
        XAI_LOG_WARN("PatternScanner: section list empty");
        return out;
    }
    auto* secs = reinterpret_cast<Script::Module::ModuleSectionInfo*>(li.data);
    int   nSec = li.count;

    constexpr int kMaxHitsPerPattern = 12;

    // 预处理用户关键字：分为 hex pattern 与字符串两类
    struct UserPat {
        std::string          raw;
        bool                 isHex;
        std::vector<uint8_t> bytes;
        std::vector<bool>    mask;            // 仅 isHex 用
        std::vector<uint8_t> utf16;           // 仅字符串用：UTF-16LE 形式
    };
    std::vector<UserPat> userPats;
    userPats.reserve(userKeywords.size());
    for (const auto& kw : userKeywords) {
        if (kw.empty()) continue;
        UserPat up;
        up.raw   = kw;
        up.isHex = false;
        if (looksLikeHexPattern(kw)) {
            if (parseHexPattern(kw, up.bytes, up.mask)) {
                up.isHex = true;
            }
        }
        if (!up.isHex) {
            up.bytes.assign(kw.begin(), kw.end());
            up.mask.assign(up.bytes.size(), true);
            up.utf16 = asciiToUtf16LE(kw);
        }
        userPats.push_back(std::move(up));
    }

    for (int s = 0; s < nSec; ++s) {
        const auto& sec = secs[s];
        if (sec.size == 0 || sec.size > 64ull * 1024 * 1024) continue;
        std::string secName = sec.name;

        std::vector<uint8_t> buf(sec.size);
        duint sizeRead = 0;
        if (!Script::Memory::Read(sec.addr, buf.data(), sec.size, &sizeRead) ||
            sizeRead == 0) continue;
        if (sizeRead < buf.size()) buf.resize(sizeRead);

        for (const auto& pat : kPatterns) {
            std::vector<size_t> hits;
            findAll(buf, pat.bytes, pat.len, hits, kMaxHitsPerPattern);
            for (size_t off : hits) {
                HeuristicHit h;
                h.kind     = HitKind::Pattern;
                h.va       = sec.addr + off;
                h.refVa    = 0;
                h.label    = pat.name;
                h.category = pat.category;
                h.score    = pat.score;
                h.evidence = "@" + secName;
                out.push_back(h);
            }
        }

        // 内嵌 PE
        std::vector<size_t> peHits;
        findEmbeddedPE(buf, peHits, 4);
        for (size_t off : peHits) {
            HeuristicHit h;
            h.kind     = HitKind::Pattern;
            h.va       = sec.addr + off;
            h.label    = "Embedded_PE";
            h.category = "embedded-blob";
            h.score    = 95;
            h.evidence = "MZ+PE @" + secName;
            out.push_back(h);
        }

        // 用户关键字 pattern / 字符串
        for (const auto& up : userPats) {
            std::vector<size_t> hits;
            findAllMasked(buf, up.bytes, up.mask, hits, kMaxHitsPerPattern);
            for (size_t off : hits) {
                HeuristicHit h;
                h.kind     = HitKind::Pattern;
                h.va       = sec.addr + off;
                h.label    = up.isHex
                    ? (std::string("user-hex: ") + up.raw)
                    : (std::string("user-str: ") + up.raw);
                h.category = "user-keyword";
                h.score    = 75;
                h.evidence = (up.isHex ? std::string("hex-pattern @") : std::string("ascii @")) + secName;
                out.push_back(h);
            }
            if (!up.isHex && !up.utf16.empty()) {
                std::vector<bool> wmask(up.utf16.size(), true);
                std::vector<size_t> whits;
                findAllMasked(buf, up.utf16, wmask, whits, kMaxHitsPerPattern);
                for (size_t off : whits) {
                    HeuristicHit h;
                    h.kind     = HitKind::Pattern;
                    h.va       = sec.addr + off;
                    h.label    = std::string("user-str(w): ") + up.raw;
                    h.category = "user-keyword";
                    h.score    = 75;
                    h.evidence = "utf16le @" + secName;
                    out.push_back(h);
                }
            }
        }
    }

    if (li.data) BridgeFree(li.data);

    std::sort(out.begin(), out.end(),
              [](const HeuristicHit& a, const HeuristicHit& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.va < b.va;
              });

    XAI_LOG_INFO("PatternScanner: hits={}", static_cast<int>(out.size()));
    return out;
}

}  // namespace x64ai
