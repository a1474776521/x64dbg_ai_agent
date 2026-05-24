// locator/api_scanner.cpp
#include "locator/api_scanner.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#include "_plugins.h"
#include "_scriptapi_module.h"
#include "bridgemain.h"

#include "util/logging.h"

namespace x64ai {

namespace {

// 把 API 名归一化：去掉末尾的 'A'/'W'，去掉 "Ex" 后缀，转小写
std::string normalizeApiName(const std::string& raw)
{
    std::string s = raw;
    // 去掉模块前缀 "kernel32.IsDebuggerPresent" -> "IsDebuggerPresent"
    auto dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(dot + 1);

    // 末尾 'A'/'W'
    if (s.size() > 1) {
        char c = s.back();
        if (c == 'A' || c == 'W') s.pop_back();
    }
    // 末尾 "Ex"
    if (s.size() > 2 && s.substr(s.size() - 2) == "Ex") {
        s.resize(s.size() - 2);
    }
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct ApiRule {
    const char* category;
    int         score;
    const char* names;   // 以 '|' 分隔的归一化小写名列表
};

// 内置敏感 API 库（归一化后小写）
const ApiRule kRules[] = {
    // 反调试 / 反分析
    { "anti-debug", 90,
      "isdebuggerpresent|checkremotedebuggerpresent|ntqueryinformationprocess|"
      "ntsetinformationthread|outputdebugstring|debugactiveprocess|"
      "ntcontinue|getthreadcontext|setthreadcontext|"
      "ntqueryobject|ntqueryvirtualmemory|"
      "rtlqueryproc|isprocessorfeaturepresent|"
      "zwsetinformationthread|zwqueryinformationprocess|"
      "findwindow|enumwindows|getforegroundwindow|"
      "blockinput|switchdesktop"
    },
    // 反 VM / 沙箱
    { "anti-vm", 80,
      "cpuid|getmodulehandle|gettickcount|queryperformancecounter|"
      "ntquerysysteminformation|getnativesysteminfo|getsysteminfo"
    },
    // 加密 / 哈希
    { "crypto", 75,
      "cryptencrypt|cryptdecrypt|cryptderivekey|cryptgenkey|cryptimportkey|"
      "cryptcreatehash|crypthashdata|cryptverifysignature|cryptacquirecontext|"
      "bcryptencrypt|bcryptdecrypt|bcryptderivekey|bcrypthashdata|bcryptgenrandom|"
      "ncryptencrypt|ncryptdecrypt|"
      "advapi32.cryptprotectdata|cryptprotectdata|cryptunprotectdata"
    },
    // 进程注入 / 操作其他进程
    { "inject", 95,
      "openprocess|virtualallocex|writeprocessmemory|readprocessmemory|"
      "createremotethread|ntcreatethreadex|rtlcreateuserthread|"
      "queueuserapc|setwindowshookex|"
      "ntmapviewofsection|zwmapviewofsection|"
      "ntwritevirtualmemory|zwwritevirtualmemory|"
      "ntunmapviewofsection|"
      "wow64setthreadcontext"
    },
    // 网络
    { "network", 60,
      "wsastartup|socket|connect|send|recv|wsaconnect|wsasend|wsarecv|"
      "internetopen|internetconnect|internetreadfile|internetwritefile|"
      "httpopenrequest|httpsendrequest|httpqueryinfo|"
      "winhttpopen|winhttpconnect|winhttpsendrequest|winhttpreaddata|"
      "urldownloadtofile|"
      "gethostbyname|getaddrinfo|inet_addr"
    },
    // 文件 / 注册表
    { "fs-reg", 50,
      "createfile|writefile|readfile|deletefile|movefile|copyfile|"
      "regopenkey|regcreatekey|regsetvalue|regqueryvalue|regdeletekey|"
      "regdeletevalue|regenumkey|regenumvalue|"
      "shfileoperation|shellexecute"
    },
    // 进程 / 模块 / 动态加载
    { "process-exec", 70,
      "createprocess|winexec|shellexecute|"
      "loadlibrary|getprocaddress|freelibrary|"
      "ntcreateprocess|ntcreateuserprocess|"
      "createtoolhelp32snapshot|process32first|process32next|"
      "module32first|module32next|"
      "ntsetinformationprocess|adjusttokenprivileges|"
      "openprocesstoken|lookupprivilegevalue"
    },
    // 内存权限 / shellcode 准备
    { "mem-protect", 65,
      "virtualalloc|virtualprotect|virtualfree|"
      "ntallocatevirtualmemory|ntprotectvirtualmemory|"
      "heapcreate|heapalloc|"
      "createfilemapping|mapviewoffile|"
      "ntcreatesection"
    },
    // GUI / 防作弊探测
    { "gui-probe", 30,
      "getwindowtext|getclassname|enumwindows|"
      "findwindow|sendmessage|postmessage"
    },
};

// 把规则展开成 hash map，O(1) 查询
const std::unordered_map<std::string, std::pair<std::string, int>>& rulesIndex()
{
    static const std::unordered_map<std::string, std::pair<std::string, int>> m = []{
        std::unordered_map<std::string, std::pair<std::string, int>> r;
        for (const auto& rule : kRules) {
            std::string s = rule.names;
            size_t i = 0;
            while (i < s.size()) {
                size_t j = s.find('|', i);
                if (j == std::string::npos) j = s.size();
                std::string name = s.substr(i, j - i);
                if (!name.empty()) {
                    r[name] = {rule.category, rule.score};
                }
                i = j + 1;
            }
        }
        return r;
    }();
    return m;
}

}  // namespace

std::pair<std::string, int> ApiScanner::classify(const std::string& apiName)
{
    auto norm = normalizeApiName(apiName);
    if (norm.empty()) return {"", 0};
    const auto& idx = rulesIndex();
    auto it = idx.find(norm);
    if (it == idx.end()) return {"", 0};
    return it->second;
}

std::vector<HeuristicHit> ApiScanner::scan(int maxXrefsPerApi,
                                           const std::vector<std::string>& userKeywords)
{
    std::vector<HeuristicHit> out;

    if (!DbgIsDebugging()) {
        XAI_LOG_WARN("ApiScanner::scan called while not debugging");
        return out;
    }

    Script::Module::ModuleInfo mod{};
    if (!Script::Module::GetMainModuleInfo(&mod)) {
        XAI_LOG_WARN("ApiScanner: GetMainModuleInfo failed");
        return out;
    }

    ListInfo li{};
    if (!Script::Module::GetImports(&mod, &li) || li.count <= 0 || !li.data) {
        XAI_LOG_WARN("ApiScanner: GetImports returned no data");
        return out;
    }

    auto* imports = reinterpret_cast<Script::Module::ModuleImport*>(li.data);
    int total = li.count;
    int matched = 0;

    for (int i = 0; i < total; ++i) {
        const auto& imp = imports[i];
        std::string apiName = imp.name[0] ? imp.name : "";
        if (apiName.empty()) continue;

        auto [category, score] = classify(apiName);
        std::string kwHit;
        if (score == 0 && !userKeywords.empty()) {
            // 用归一化（去模块前缀/后缀，小写）名做 substring 匹配
            std::string norm = normalizeApiName(apiName);
            // 同时也允许匹配原始 apiName（小写），以覆盖 W/A/Ex 字面量
            std::string raw = apiName;
            std::transform(raw.begin(), raw.end(), raw.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            for (const auto& k : userKeywords) {
                if (k.empty()) continue;
                if (norm.find(k) != std::string::npos ||
                    raw.find(k) != std::string::npos) {
                    kwHit = k;
                    category = "user-keyword";
                    score = 70;
                    break;
                }
            }
        }
        if (score == 0) continue;
        ++matched;

        std::string baseEv = "IAT slot";
        if (!kwHit.empty()) baseEv += " [kw:" + kwHit + "]";

        // 主条目：IAT slot
        HeuristicHit hit;
        hit.kind     = HitKind::Api;
        hit.va       = imp.iatVa;
        hit.refVa    = 0;
        hit.label    = apiName;
        hit.category = category;
        hit.score    = score;
        hit.evidence = baseEv;
        out.push_back(hit);

        // xref：每个调用点单列一条（更易跳转）
        if (maxXrefsPerApi > 0) {
            size_t xc = DbgGetXrefCountAt(imp.iatVa);
            if (xc > 0) {
                XREF_INFO xi{};
                if (DbgXrefGet(imp.iatVa, &xi) && xi.references) {
                    int cap = std::min<int>(maxXrefsPerApi,
                                            static_cast<int>(xi.refcount));
                    for (int k = 0; k < cap; ++k) {
                        HeuristicHit ch;
                        ch.kind     = HitKind::Api;
                        ch.va       = xi.references[k].addr;
                        ch.refVa    = imp.iatVa;
                        ch.label    = apiName;
                        ch.category = category;
                        ch.score    = std::min(100, score + 5);  // 调用点比 slot 略加分
                        ch.evidence = kwHit.empty()
                            ? std::string("call/jmp to IAT")
                            : std::string("call/jmp to IAT [kw:") + kwHit + "]";
                        out.push_back(ch);
                    }
                    if (xi.references) BridgeFree(xi.references);
                }
            }
        }
    }

    if (li.data) BridgeFree(li.data);

    // 按分数降序，分数相同按 label 字典序
    std::sort(out.begin(), out.end(),
              [](const HeuristicHit& a, const HeuristicHit& b) {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.label != b.label) return a.label < b.label;
                  return a.va < b.va;
              });

    XAI_LOG_INFO("ApiScanner: scanned {} imports, matched {} sensitive, hits={}",
                 total, matched, static_cast<int>(out.size()));
    return out;
}

}  // namespace x64ai
