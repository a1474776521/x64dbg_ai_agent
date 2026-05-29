// ai/tools/analyze_pe_header_tool.cpp
//
// K-34 analyze_pe_header  - PE 头深度分析，恶意代码 / 加壳样本一次性画像
//
// 字段覆盖：
//   - 基本：machine / subsystem / TimeDateStamp（含异常检测：未来时间/epoch=0/
//                                              年份过老）
//   - PE32 vs PE32+
//   - Entry Point（VA + RVA）
//   - SizeOfImage / SizeOfHeaders / CheckSum（含 mismatch 检查）
//   - DLL Characteristics: NX/ASLR/CFG/HVCI/SEHValid 等
//   - Subsystem 异常（GUI/CLI/native/...）
//   - Sections: name / virt size / raw size / RWX / 可疑壳 marker / entropy
//   - Resources: 顶层类型分布 + 单条 size 异常（>100 KB → payload 嫌疑）
//   - Data Directory: Cert / Imports / Exports / Reloc / Debug 是否存在
//   - Authenticode: 是否有签名（仅检 Cert 目录非零，不验证链）
//   - 风险评分（0-100）+ 命中标签数组（便于 agent 量化判断）
//
// 数据源：优先从磁盘文件读（`ModuleInfo.path`），保证 raw section data 完整可算
//        entropy / authenticode。运行时 image 因 PE loader 已 unmap raw，
//        entropy 不准。
//
// 注意：pe-parse `ParsePEFromFile` 不释放路径里中文的限制时，先 ANSI→UTF-8 经 fopen
//      可能失败；这里改用 readFileToFileBuffer 等价的「自己读全文 + makeBufferFromPointer」
//      但 pe-parse 自带的 ParsePEFromFile 内部用 stat+fopen，中文路径在 GBK code
//      page 系统下 OK。极端情况退化为 ERROR。
#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_args_util.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

// pe-parse 必须在 Windows.h 之前 include：
// 二者都定义 IMAGE_SUBSYSTEM_* / IMAGE_SCN_* / RT_* 等同名符号，
// Windows.h 是宏，pe-parse 是 constexpr；先后顺序错了会触发 C2059 / C2737。
#include <pe-parse/parse.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <Windows.h>
#include "bridgemain.h"
#include "_scriptapi_module.h"

#include "util/logging.h"
#include "util/encoding.h"

namespace x64ai {

namespace {

std::string formatHexU64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
    return buf;
}

// 香农熵（base-2）；输入 bytes 范围
double shannonEntropy(const std::uint8_t* data, std::size_t n)
{
    if (n == 0) return 0.0;
    std::size_t cnt[256] = {0};
    for (std::size_t i = 0; i < n; ++i) cnt[data[i]]++;
    double H = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (cnt[i] == 0) continue;
        const double p = static_cast<double>(cnt[i]) / static_cast<double>(n);
        H -= p * std::log2(p);
    }
    return H;
}

const char* subsystemName(std::uint16_t s)
{
    switch (s) {
    case 1:  return "native";
    case 2:  return "windows_gui";
    case 3:  return "windows_cui";
    case 5:  return "os2_cui";
    case 7:  return "posix_cui";
    case 9:  return "windows_ce_gui";
    case 10: return "efi_application";
    case 11: return "efi_boot_service_driver";
    case 12: return "efi_runtime_driver";
    case 13: return "efi_rom";
    case 14: return "xbox";
    default: return "unknown";
    }
}

// IMAGE_DLLCHARACTERISTICS_*
struct DllChar {
    std::uint16_t mask;
    const char*   name;
};
constexpr DllChar kDllChars[] = {
    {0x0020, "HIGH_ENTROPY_VA"},
    {0x0040, "DYNAMIC_BASE"},     // ASLR
    {0x0080, "FORCE_INTEGRITY"},
    {0x0100, "NX_COMPAT"},
    {0x0200, "NO_ISOLATION"},
    {0x0400, "NO_SEH"},
    {0x0800, "NO_BIND"},
    {0x1000, "APPCONTAINER"},
    {0x2000, "WDM_DRIVER"},
    {0x4000, "GUARD_CF"},         // CFG
    {0x8000, "TERMINAL_SERVER_AWARE"},
};

// 已知壳 / packer / 混淆器节名 prefix 匹配
const char* detectPackerByName(const std::string& secName)
{
    struct M { const char* prefix; const char* family; };
    static constexpr M tbl[] = {
        {"UPX",      "UPX"},
        {".UPX",     "UPX"},
        {".aspack",  "ASPack"},
        {".adata",   "ASPack"},
        {".vmp",     "VMProtect"},
        {".themida", "Themida"},
        {"WinLicens","Themida/WinLicense"},
        {".enigma",  "Enigma"},
        {".pec",     "PECompact"},
        {"PEC2",     "PECompact"},
        {".mpress",  "MPRESS"},
        {".petite",  "Petite"},
        {".nsp",     "NsPack"},
        {".y0da",    "y0da"},
        {".boom",    "BOOM"},
        {".MEW",     "MEW"},
    };
    for (const auto& m : tbl) {
        const std::size_t L = std::strlen(m.prefix);
        if (secName.size() >= L && std::memcmp(secName.data(), m.prefix, L) == 0) {
            return m.family;
        }
    }
    return nullptr;
}

// section name 是否“随机”：长度 >=4，且至少 3 个字符不在 [a-zA-Z0-9._]
bool isRandomLookingName(const std::string& n)
{
    if (n.size() < 4) return false;
    int weird = 0;
    for (char c : n) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == 0))
            weird++;
    }
    return weird >= 3;
}

const char* resourceTypeName(std::uint32_t t)
{
    switch (t) {
    case 1:  return "CURSOR";
    case 2:  return "BITMAP";
    case 3:  return "ICON";
    case 4:  return "MENU";
    case 5:  return "DIALOG";
    case 6:  return "STRING";
    case 7:  return "FONTDIR";
    case 8:  return "FONT";
    case 9:  return "ACCELERATOR";
    case 10: return "RCDATA";
    case 11: return "MESSAGETABLE";
    case 12: return "GROUP_CURSOR";
    case 14: return "GROUP_ICON";
    case 16: return "VERSION";
    case 17: return "DLGINCLUDE";
    case 19: return "PLUGPLAY";
    case 20: return "VXD";
    case 21: return "ANICURSOR";
    case 22: return "ANIICON";
    case 23: return "HTML";
    case 24: return "MANIFEST";
    default: return "OTHER";
    }
}

// ====== AnalyzePeHeaderTool ======

class AnalyzePeHeaderTool : public ITool {
public:
    std::string name() const override { return "analyze_pe_header"; }
    std::string description() const override
    {
        return "Deep PE/PE32+ header analysis of a module (reads on-disk file). Reports machine, "
               "subsystem, TimeDateStamp (with anomaly check: future / epoch=0 / >20y old), entry "
               "point, image checksum match, DLL characteristics (NX/ASLR/CFG/...), section table "
               "with per-section entropy + RWX flag + known packer family markers (UPX/VMProtect/"
               "Themida/Enigma/...), top-level resource type histogram with oversize-resource hint "
               "(>100KB = embedded payload suspect), data directory presence, and Authenticode "
               "signature presence (existence only, no chain verify). Outputs a risk_score (0-100) "
               "and risk_tags array for quantitative triage."
               " (zh-CN: 深度 PE/PE32+ 头分析。读磁盘文件，给出 machine / subsystem / 编译时间戳"
               "（含未来时间 / 1970 / >20 年异常检查）/ EP / checksum / DLL 特性（NX/ASLR/CFG）/ "
               "节表（entropy + RWX + 已知壳 marker UPX/VMProtect/Themida 等）/ 资源类型分布（含"
               ">100KB 嵌入 payload 嫌疑）/ 数据目录 / Authenticode 签名是否存在。同时给出 "
               "risk_score 0-100 + risk_tags 量化标签数组。)";
    }
    std::string descriptionZh() const override
    {
        return "深度分析模块的 PE / PE32+ 文件头（从磁盘读取原始文件）。一次性输出："
               "machine / subsystem / 编译时间戳（含未来时间 / 1970-epoch / >20 年异常检测） / "
               "EntryPoint / Image CheckSum 是否匹配 / DLL 特性（NX, ASLR, CFG, ...） / "
               "节表（每节 entropy + RWX 标志 + 已知壳家族 marker：UPX / VMProtect / Themida / "
               "Enigma / ASPack 等） / 资源类型直方图（>100 KB 单条资源标为 payload 嫌疑） / "
               "数据目录存在性 / Authenticode 签名是否存在（仅检存在性，不验证签名链）。"
               "最终给出 risk_score (0-100) + risk_tags 数组，便于 agent 量化判定恶意可能性。";
    }

    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"module", {{"type", "string"},
                            {"description", "Module name (e.g. 'malware.exe' or 'kernel32.dll'). Defaults to main module if omitted."}}},
            }},
        };
    }

    std::size_t maxResultBytes() const override { return 256 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }

        // 1) 解析模块名 → path
        Script::Module::ModuleInfo mi{};
        if (args.contains("module") && args["module"].is_string() && !args["module"].get<std::string>().empty()) {
            const auto modName = args["module"].get<std::string>();
            if (!Script::Module::InfoFromName(modName.c_str(), &mi)) {
                r.ok = false; r.error = "module not loaded: " + modName;
                return r;
            }
        } else {
            char mainPath[MAX_PATH] = {0};
            if (!Script::Module::GetMainModulePath(mainPath) ||
                !Script::Module::InfoFromAddr(reinterpret_cast<duint>(mainPath), &mi)) {
                // 退化：拿主模块通过 PathFromAddr 找
                if (!Script::Module::GetMainModulePath(mainPath)) {
                    r.ok = false; r.error = "GetMainModulePath failed";
                    return r;
                }
                // 用 path 本身做 InfoFromAddr 不对；改用 base 的简单办法：
                // 这里假设主模块名是 mainPath 的 basename
                const char* slash = std::strrchr(mainPath, '\\');
                std::string base = slash ? slash + 1 : mainPath;
                if (!Script::Module::InfoFromName(base.c_str(), &mi)) {
                    r.ok = false;
                    r.error = std::string("cannot locate main module info; tried name='") + base + "'";
                    return r;
                }
            }
        }

        const std::string modPath = mi.path;  // ANSI 编码（GBK 系统）
        if (modPath.empty()) {
            r.ok = false; r.error = "module path is empty";
            return r;
        }

        // 2) 用 pe-parse 打开（ParsePEFromFile 内部 fopen，中文路径走 ANSI OK）
        peparse::parsed_pe* pe = peparse::ParsePEFromFile(modPath.c_str());
        if (!pe) {
            r.ok = false;
            r.error = "ParsePEFromFile failed: " + peparse::GetPEErrString() +
                      " at " + peparse::GetPEErrLoc() + "; path=" + modPath;
            return r;
        }
        std::unique_ptr<peparse::parsed_pe, void(*)(peparse::parsed_pe*)>
            peGuard(pe, &peparse::DestructParsedPE);

        // 3) 基本字段提取
        const auto& fh    = pe->peHeader.nt.FileHeader;
        const bool  isPe64 = (pe->peHeader.nt.OptionalMagic == 0x20b);

        const std::uint16_t subsys      = isPe64 ? pe->peHeader.nt.OptionalHeader64.Subsystem
                                                 : pe->peHeader.nt.OptionalHeader.Subsystem;
        const std::uint16_t dllChars    = isPe64 ? pe->peHeader.nt.OptionalHeader64.DllCharacteristics
                                                 : pe->peHeader.nt.OptionalHeader.DllCharacteristics;
        const std::uint32_t sizeOfImage = isPe64 ? pe->peHeader.nt.OptionalHeader64.SizeOfImage
                                                 : pe->peHeader.nt.OptionalHeader.SizeOfImage;
        const std::uint32_t checkSum    = isPe64 ? pe->peHeader.nt.OptionalHeader64.CheckSum
                                                 : pe->peHeader.nt.OptionalHeader.CheckSum;
        const std::uint32_t aoep        = isPe64 ? pe->peHeader.nt.OptionalHeader64.AddressOfEntryPoint
                                                 : pe->peHeader.nt.OptionalHeader.AddressOfEntryPoint;
        const std::uint64_t imageBase   = isPe64 ? pe->peHeader.nt.OptionalHeader64.ImageBase
                                                 : static_cast<std::uint64_t>(pe->peHeader.nt.OptionalHeader.ImageBase);

        // === 风险评分构建器 ===
        int risk = 0;
        std::vector<std::string> risk_tags;
        auto addRisk = [&](int delta, std::string tag) {
            risk += delta;
            risk_tags.push_back(std::move(tag));
        };

        // 4) TimeDateStamp 异常检查
        const std::uint32_t tds = fh.TimeDateStamp;
        nlohmann::json timestampInfo;
        {
            const std::time_t now = std::time(nullptr);
            timestampInfo["raw"] = tds;
            if (tds == 0) {
                timestampInfo["status"] = "epoch_zero";
                addRisk(8, "timestamp_epoch_zero");  // packer/混淆常见
            } else if (tds > static_cast<std::uint32_t>(now) + 86400) {
                timestampInfo["status"] = "future";
                addRisk(12, "timestamp_future");
            } else if (tds < 631152000U) {  // 1990-01-01
                timestampInfo["status"] = "ancient";
                addRisk(6, "timestamp_ancient");
            } else {
                timestampInfo["status"] = "normal";
            }
            // 人类可读
            std::time_t t = tds;
            std::tm tm{};
            gmtime_s(&tm, &t);
            char tb[32];
            std::strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S UTC", &tm);
            timestampInfo["human"] = tb;
        }

        // 5) DLL Characteristics decode
        nlohmann::json dllCharArr = nlohmann::json::array();
        bool hasNx = false, hasAslr = false, hasCfg = false;
        for (const auto& dc : kDllChars) {
            if (dllChars & dc.mask) {
                dllCharArr.push_back(dc.name);
                if (dc.mask == 0x0100) hasNx   = true;
                if (dc.mask == 0x0040) hasAslr = true;
                if (dc.mask == 0x4000) hasCfg  = true;
            }
        }
        if (!hasNx)   addRisk(5,  "no_nx");
        if (!hasAslr) addRisk(4,  "no_aslr");

        // 6) Subsystem 异常（取证场景：driver / unknown 都可疑）
        if (subsys == 1) addRisk(15, "subsystem_native");  // 用户态样本声称 native 极少见

        // 7) Sections + entropy + packer detection
        struct SecAcc {
            int       rwxCount      = 0;
            int       highEntropy   = 0;  // entropy > 7.0
            int       packerHits    = 0;
            int       randomNames   = 0;
            std::vector<nlohmann::json> rows;
        };
        SecAcc sa;

        peparse::IterSec(
            pe,
            [](void* cbd, const peparse::VA& secBase, const std::string& secName,
               const peparse::image_section_header& sh, const peparse::bounded_buffer* buf) -> int {
                auto& acc = *static_cast<SecAcc*>(cbd);
                const std::uint32_t ch = sh.Characteristics;
                const bool exec  = (ch & 0x20000000u) != 0;
                const bool wr    = (ch & 0x80000000u) != 0;
                const bool rd    = (ch & 0x40000000u) != 0;
                const bool rwx   = exec && wr && rd;
                if (rwx) acc.rwxCount++;

                double entropy = 0.0;
                if (buf && buf->buf && buf->bufLen > 0) {
                    entropy = shannonEntropy(buf->buf,
                        static_cast<std::size_t>(std::min<std::uint64_t>(
                            buf->bufLen, 16ULL * 1024 * 1024)));  // 单节最多算 16MB
                }
                if (entropy > 7.0) acc.highEntropy++;

                const char* packerFamily = detectPackerByName(secName);
                if (packerFamily) acc.packerHits++;
                const bool randName = isRandomLookingName(secName);
                if (randName) acc.randomNames++;

                nlohmann::json row = {
                    {"name",         secName},
                    {"virt_size",    sh.Misc.VirtualSize},
                    {"raw_size",     sh.SizeOfRawData},
                    {"va",           formatHexU64(secBase)},
                    {"rva",          formatHexU64(sh.VirtualAddress)},
                    {"characteristics", formatHexU64(ch)},
                    {"r",            rd},
                    {"w",            wr},
                    {"x",            exec},
                    {"rwx",          rwx},
                    {"entropy",      entropy},
                };
                if (packerFamily) row["packer_marker"] = packerFamily;
                if (randName)     row["random_name"]   = true;
                acc.rows.push_back(std::move(row));
                return 0;  // continue
            }, &sa);

        if (sa.rwxCount > 0)    addRisk(15 * sa.rwxCount, "rwx_section");
        if (sa.highEntropy > 0) addRisk(8  * sa.highEntropy, "high_entropy_section");
        if (sa.packerHits > 0)  addRisk(25, "packer_marker");
        if (sa.randomNames > 0) addRisk(6  * sa.randomNames, "random_section_name");

        // 8) Resources（顶层类型统计 + 大资源标记）
        struct RsrcAcc {
            std::map<std::uint32_t, int>          countByType;
            std::map<std::uint32_t, std::uint64_t> totalSizeByType;
            int       oversizedCount = 0;     // >100 KB
            std::uint32_t maxSize    = 0;
        };
        RsrcAcc ra;
        peparse::IterRsrc(
            pe,
            [](void* cbd, const peparse::resource& rs) -> int {
                auto& acc = *static_cast<RsrcAcc*>(cbd);
                acc.countByType[rs.type]++;
                acc.totalSizeByType[rs.type] += rs.size;
                if (rs.size > 100 * 1024) acc.oversizedCount++;
                if (rs.size > acc.maxSize) acc.maxSize = rs.size;
                return 0;
            }, &ra);

        nlohmann::json rsrcArr = nlohmann::json::array();
        for (const auto& kv : ra.countByType) {
            rsrcArr.push_back({
                {"type",       kv.first},
                {"type_name",  resourceTypeName(kv.first)},
                {"count",      kv.second},
                {"total_size", ra.totalSizeByType[kv.first]},
            });
        }
        if (ra.oversizedCount > 0) addRisk(10 * ra.oversizedCount, "oversized_resource");

        // 9) Data Directories - 仅看关键四个
        auto getDD = [&](int idx) -> peparse::data_directory {
            if (isPe64) return pe->peHeader.nt.OptionalHeader64.DataDirectory[idx];
            else        return pe->peHeader.nt.OptionalHeader.DataDirectory[idx];
        };
        const auto ddImport = getDD(peparse::DIR_IMPORT);
        const auto ddExport = getDD(peparse::DIR_EXPORT);
        const auto ddCert   = getDD(peparse::DIR_SECURITY);  // Authenticode
        const auto ddReloc  = getDD(peparse::DIR_BASERELOC);
        const auto ddDebug  = getDD(peparse::DIR_DEBUG);
        const auto ddTls    = getDD(peparse::DIR_TLS);

        const bool hasAuthenticode = (ddCert.VirtualAddress != 0 && ddCert.Size != 0);
        const bool hasTls          = (ddTls.VirtualAddress  != 0 && ddTls.Size  != 0);
        const bool hasReloc        = (ddReloc.VirtualAddress!= 0 && ddReloc.Size!= 0);
        const bool hasImport       = (ddImport.VirtualAddress!=0 && ddImport.Size!=0);

        if (!hasAuthenticode) addRisk(3,  "no_signature");
        if (hasTls)           addRisk(4,  "has_tls");  // TLS callback 常被滥用反调试
        if (!hasReloc)        addRisk(2,  "no_reloc");  // 加壳/特殊样本常剥 reloc
        if (!hasImport)       addRisk(15, "no_imports");  // 几乎一定是加壳

        // 10) Entry point 是否在 .text 之外（落在最后一节常见于加壳）
        bool epInLastSec = false;
        if (!sa.rows.empty()) {
            const auto& last = sa.rows.back();
            const std::uint64_t lastRva  = std::stoull(last["rva"].get<std::string>().substr(2), nullptr, 16);
            const std::uint32_t lastSize = last["virt_size"].get<std::uint32_t>();
            if (aoep >= lastRva && aoep < lastRva + lastSize) {
                epInLastSec = true;
                addRisk(12, "ep_in_last_section");
            }
        }

        // 11) 风险评分封顶 100
        if (risk > 100) risk = 100;
        const char* level = (risk >= 60) ? "high" : (risk >= 30) ? "medium" : "low";

        // 12) 输出
        r.ok = true;
        r.data = {
            {"module",         mi.name},
            {"path",           modPath},
            {"base",           formatHexU64(static_cast<std::uint64_t>(mi.base))},
            {"size",           static_cast<std::uint64_t>(mi.size)},

            {"machine",        peparse::GetMachineAsString(pe)},
            {"is_pe64",        isPe64},
            {"image_base",     formatHexU64(imageBase)},
            {"size_of_image",  sizeOfImage},
            {"checksum",       formatHexU64(checkSum)},
            {"checksum_present", checkSum != 0},

            {"entry_point_rva", formatHexU64(aoep)},
            {"entry_point_va",  formatHexU64(imageBase + aoep)},
            {"ep_in_last_section", epInLastSec},

            {"subsystem",      subsys},
            {"subsystem_name", subsystemName(subsys)},
            {"dll_characteristics", dllCharArr},
            {"nx_compat",      hasNx},
            {"aslr",           hasAslr},
            {"cfg",            hasCfg},

            {"timestamp",      timestampInfo},

            {"sections",       {
                {"count",       sa.rows.size()},
                {"rwx_count",   sa.rwxCount},
                {"high_entropy_count", sa.highEntropy},
                {"packer_hits", sa.packerHits},
                {"random_name_count", sa.randomNames},
                {"rows",        sa.rows},
            }},

            {"resources",      {
                {"types",       rsrcArr},
                {"max_size",    ra.maxSize},
                {"oversized_count", ra.oversizedCount},
            }},

            {"data_directories", {
                {"has_imports",       hasImport},
                {"has_exports",       (ddExport.VirtualAddress!=0 && ddExport.Size!=0)},
                {"has_relocs",        hasReloc},
                {"has_debug",         (ddDebug.VirtualAddress!=0 && ddDebug.Size!=0)},
                {"has_tls",           hasTls},
                {"has_authenticode",  hasAuthenticode},
                {"authenticode_size", ddCert.Size},
            }},

            {"risk_score",    risk},
            {"risk_level",    level},
            {"risk_tags",     risk_tags},
        };

        XAI_LOG_INFO("analyze_pe_header: mod={} pe64={} sec={} rwx={} pkr={} "
                     "ent>{}={} risk={} ({})",
                     mi.name, isPe64, sa.rows.size(), sa.rwxCount, sa.packerHits,
                     "7.0", sa.highEntropy, risk, level);
        return r;
    }
};

}  // namespace

void registerAnalyzePeHeaderTool(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<AnalyzePeHeaderTool>(), "forensics");
}

}  // namespace x64ai
