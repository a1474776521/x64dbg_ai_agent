// locator/string_scanner.cpp
#include "locator/string_scanner.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "_plugins.h"
#include "_scriptapi_memory.h"
#include "_scriptapi_module.h"
#include "bridgemain.h"

#include "util/logging.h"

namespace x64ai {

namespace {

bool isPrintableAscii(uint8_t c)
{
    return c == '\t' || c == '\r' || c == '\n' || (c >= 0x20 && c <= 0x7E);
}

// 简单关键词加权表
struct KwRule { const char* needle; const char* category; int score; };

const KwRule kKwRules[] = {
    // 安全 / 反作弊
    { "license",    "license",    85 },
    { "trial",      "license",    80 },
    { "expir",      "license",    75 },
    { "register",   "license",    70 },
    { "serial",     "license",    80 },
    { "activation", "license",    80 },
    { "key",        "license",    55 },
    { "crack",      "anti-tamper",90 },
    { "patch",      "anti-tamper",60 },
    { "debug",      "anti-debug", 70 },
    { "ollydbg",    "anti-debug", 95 },
    { "x64dbg",     "anti-debug", 95 },
    { "x32dbg",     "anti-debug", 95 },
    { "ida.exe",    "anti-debug", 95 },
    { "windbg",     "anti-debug", 90 },
    { "vmware",     "anti-vm",    85 },
    { "vbox",       "anti-vm",    85 },
    { "virtualbox", "anti-vm",    85 },
    { "qemu",       "anti-vm",    85 },
    { "sandbox",    "anti-vm",    80 },

    // 网络相关
    { "http://",    "network",    65 },
    { "https://",   "network",    65 },
    { "mozilla/",   "network",    55 },
    { "user-agent", "network",    60 },
    { ".onion",     "network",    90 },
    { "irc.",       "network",    70 },

    // SQL / config / 凭据
    { "select ",    "sql",        70 },
    { "insert ",    "sql",        70 },
    { "update ",    "sql",        65 },
    { "delete ",    "sql",        65 },
    { "password",   "credential", 85 },
    { "passwd",     "credential", 85 },
    { "username",   "credential", 60 },
    { "token",      "credential", 70 },
    { "secret",     "credential", 75 },
    { "api_key",    "credential", 90 },

    // 错误信息（常被反向定位用）
    { "error",      "error-msg",  40 },
    { "failed",     "error-msg",  40 },
    { "invalid",    "error-msg",  40 },
    { "denied",     "error-msg",  45 },

    // 加密 / 加壳
    { "aes",        "crypto",     65 },
    { "rsa",        "crypto",     65 },
    { "md5",        "crypto",     55 },
    { "sha1",       "crypto",     55 },
    { "sha256",     "crypto",     60 },
    { "upx",        "packer",     85 },
    { "vmprotect",  "packer",     90 },
    { "themida",    "packer",     90 },
    { "enigma",     "packer",     85 },

    // 注册表 / 启动项
    { "software\\microsoft\\windows\\currentversion\\run", "persistence", 90 },
    { "currentversion\\run",                                "persistence", 80 },
    { "schtasks",   "persistence",75 },
};

// 把字符串小写，用于关键词匹配
std::string toLower(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return r;
}

std::pair<std::string, int> classifyString(const std::string& text)
{
    std::string lo = toLower(text);
    std::string bestCat;
    int         bestScore = 0;
    for (const auto& r : kKwRules) {
        if (lo.find(r.needle) != std::string::npos) {
            if (r.score > bestScore) {
                bestScore = r.score;
                bestCat   = r.category;
            }
        }
    }
    return {bestCat, bestScore};
}

// 返回首个命中的用户关键字（小写）；未命中返回空串
std::string matchUserKeyword(const std::string& textLower,
                             const std::vector<std::string>& kws)
{
    for (const auto& k : kws) {
        if (!k.empty() && textLower.find(k) != std::string::npos) return k;
    }
    return {};
}

std::string makeLabel(const std::string& s)
{
    constexpr size_t kMax = 60;
    std::string out;
    out.reserve(std::min(s.size(), kMax) + 4);
    for (size_t i = 0; i < s.size() && out.size() < kMax; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\r' || c == '\n' || c == '\t') { out.push_back(' '); continue; }
        if (c < 0x20 || c == 0x7F) continue;
        out.push_back(static_cast<char>(c));
    }
    if (s.size() > kMax) out.append("…");
    return out;
}

// 从 buf 提取 ASCII 字符串，输出 (rva 起始, 文本)
void extractAscii(const std::vector<uint8_t>& buf, int minLen,
                  std::vector<std::pair<size_t, std::string>>& out, int cap)
{
    size_t i = 0;
    while (i < buf.size() && static_cast<int>(out.size()) < cap) {
        if (isPrintableAscii(buf[i])) {
            size_t start = i;
            while (i < buf.size() && isPrintableAscii(buf[i])) ++i;
            size_t len = i - start;
            if (static_cast<int>(len) >= minLen) {
                out.emplace_back(start, std::string(
                    reinterpret_cast<const char*>(&buf[start]), len));
            }
        } else {
            ++i;
        }
    }
}

// 从 buf 提取 UTF-16LE（仅基本拉丁字符）
void extractUtf16(const std::vector<uint8_t>& buf, int minLen,
                  std::vector<std::pair<size_t, std::string>>& out, int cap)
{
    if (buf.size() < 4) return;
    size_t i = 0;
    while (i + 1 < buf.size() && static_cast<int>(out.size()) < cap) {
        // 检测连续宽字符
        size_t start = i;
        std::string acc;
        while (i + 1 < buf.size()) {
            uint8_t lo = buf[i];
            uint8_t hi = buf[i + 1];
            if (hi == 0 && isPrintableAscii(lo)) {
                acc.push_back(static_cast<char>(lo));
                i += 2;
            } else {
                break;
            }
        }
        if (static_cast<int>(acc.size()) >= minLen) {
            out.emplace_back(start, std::move(acc));
        } else {
            i = start + 1;  // 退一位重试，避免漏检
        }
    }
}

}  // namespace

std::vector<HeuristicHit> StringScanner::scan(const Options& opt)
{
    std::vector<HeuristicHit> out;

    if (!DbgIsDebugging()) {
        XAI_LOG_WARN("StringScanner: not debugging");
        return out;
    }

    Script::Module::ModuleInfo mod{};
    if (!Script::Module::GetMainModuleInfo(&mod)) {
        XAI_LOG_WARN("StringScanner: GetMainModuleInfo failed");
        return out;
    }

    ListInfo li{};
    if (!Script::Module::GetMainModuleSectionList(&li) || li.count <= 0 || !li.data) {
        XAI_LOG_WARN("StringScanner: section list empty");
        return out;
    }
    auto* secs = reinterpret_cast<Script::Module::ModuleSectionInfo*>(li.data);
    int   nSec = li.count;

    int strBudget = opt.maxStrings;
    int totalStrings = 0;

    for (int s = 0; s < nSec && strBudget > 0; ++s) {
        const auto& sec = secs[s];
        if (sec.size == 0 || sec.size > 64ull * 1024 * 1024) continue;
        // 跳过纯代码节（节名包含 .text）：字符串常驻数据段
        std::string secName = sec.name;
        std::string secLo   = toLower(secName);
        if (secLo.find(".text") != std::string::npos) continue;
        if (secLo.find(".rsrc") != std::string::npos) continue;  // 资源里字符串太多噪声
        if (secLo.find(".reloc") != std::string::npos) continue;

        std::vector<uint8_t> buf(sec.size);
        duint sizeRead = 0;
        if (!Script::Memory::Read(sec.addr, buf.data(), sec.size, &sizeRead) ||
            sizeRead == 0) {
            continue;
        }
        if (sizeRead < buf.size()) buf.resize(sizeRead);

        std::vector<std::pair<size_t, std::string>> strings;
        extractAscii(buf, opt.minAsciiLen, strings, strBudget);
        extractUtf16(buf, opt.minUtf16Len, strings, strBudget);
        totalStrings += static_cast<int>(strings.size());

        for (const auto& [off, txt] : strings) {
            if (--strBudget <= 0) break;
            auto [cat, score] = classifyString(txt);

            std::string txtLo = toLower(txt);
            std::string kwHit = matchUserKeyword(txtLo, opt.userKeywords);

            uint64_t va = sec.addr + off;
            duint    vaD = static_cast<duint>(va);

            bool emit = (score > 0) || !kwHit.empty();
            if (!emit && opt.keepUnscoredWithXref && opt.maxXrefsPerStr > 0) {
                emit = (DbgGetXrefCountAt(vaD) > 0);
            }
            if (!emit) continue;

            int finalScore = (score > 0) ? score : 25;
            std::string finalCat = cat.empty() ? std::string("misc") : cat;
            std::string ev = "@" + std::string(secName);
            if (!kwHit.empty()) {
                finalScore = std::min(100, finalScore + 40);
                if (cat.empty()) finalCat = "user-keyword";
                ev += " [kw:" + kwHit + "]";
            }

            HeuristicHit hit;
            hit.kind     = HitKind::String;
            hit.va       = va;
            hit.refVa    = 0;
            hit.label    = makeLabel(txt);
            hit.category = finalCat;
            hit.score    = finalScore;
            hit.evidence = ev;
            out.push_back(hit);

            // xref 调用点
            if (opt.maxXrefsPerStr > 0) {
                size_t xc = DbgGetXrefCountAt(vaD);
                if (xc > 0) {
                    XREF_INFO xi{};
                    if (DbgXrefGet(vaD, &xi) && xi.references) {
                        int cap = std::min<int>(opt.maxXrefsPerStr,
                                                static_cast<int>(xi.refcount));
                        for (int k = 0; k < cap; ++k) {
                            HeuristicHit ch = hit;
                            ch.va    = xi.references[k].addr;
                            ch.refVa = va;
                            ch.score = std::min(100, hit.score + 5);
                            ch.evidence = "ref-from-code";
                            out.push_back(ch);
                        }
                        if (xi.references) BridgeFree(xi.references);
                    }
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

    XAI_LOG_INFO("StringScanner: scanned ~{} strings, hits={}",
                 totalStrings, static_cast<int>(out.size()));
    return out;
}

}  // namespace x64ai
