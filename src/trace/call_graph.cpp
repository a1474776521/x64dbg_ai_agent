// trace/call_graph.cpp
#include "trace/call_graph.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace x64ai {

namespace {

void dfsFind(CallNode* node, uint64_t target,
             std::vector<CallNode*>& stack,
             std::vector<CallPath>& out)
{
    for (auto& cu : node->children) {
        CallNode* c = cu.get();
        stack.push_back(c);
        if (c->addr == target) {
            CallPath p;
            p.nodes = stack;
            out.push_back(std::move(p));
        }
        dfsFind(c, target, stack, out);
        stack.pop_back();
    }
}

void dfsSummary(CallNode* n, std::size_t depth,
                std::size_t& nodes, std::size_t& maxDepth)
{
    ++nodes;
    if (depth > maxDepth) maxDepth = depth;
    for (auto& c : n->children) dfsSummary(c.get(), depth + 1, nodes, maxDepth);
}

// 从符号字符串里提取"模块名前缀"（小写）。
// 支持：
//   "ucrtbase._fputc_nolock"  -> "ucrtbase"
//   "ucrtbase+0x1707C"        -> "ucrtbase"
//   "kernel32.dll.LoadLibrary"-> "kernel32"
//   "trace_demo"              -> "" （没有点/加号 = 视为非模块前缀符号，归属用户）
std::string extractModule(const std::string& sym)
{
    if (sym.empty()) return {};
    std::size_t cut = std::string::npos;
    for (std::size_t i = 0; i < sym.size(); ++i) {
        char c = sym[i];
        if (c == '.' || c == '+' || c == ':') { cut = i; break; }
    }
    if (cut == std::string::npos) return {};
    std::string m = sym.substr(0, cut);
    // 去 .dll/.exe 后缀（少见，但稳妥）
    auto endsWith = [&](const char* suf, std::size_t sl) {
        return m.size() > sl &&
               std::equal(m.end() - sl, m.end(), suf,
                          [](char a, char b){ return std::tolower(a) == b; });
    };
    if (endsWith(".dll", 4) || endsWith(".exe", 4)) {
        m.resize(m.size() - 4);
    }
    // 转小写
    for (auto& ch : m) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return m;
}

// 系统模块清单（小写、不带后缀）
const std::unordered_set<std::string>& systemModules()
{
    static const std::unordered_set<std::string> kSet = {
        // CRT
        "ucrtbase", "msvcrt", "msvcp140", "msvcp110", "msvcp120",
        "vcruntime140", "vcruntime140_1", "concrt140",
        "api-ms-win-crt-runtime-l1-1-0",
        // 核心 Win32
        "kernel32", "kernelbase", "ntdll", "win32u",
        "user32", "gdi32", "gdi32full", "advapi32", "sechost",
        // 网络/IPC
        "ws2_32", "mswsock", "rpcrt4", "iphlpapi", "winhttp", "wininet",
        // Shell / COM
        "shell32", "shlwapi", "ole32", "oleaut32", "combase", "uxtheme",
        // 安全 / 加密
        "bcrypt", "bcryptprimitives", "crypt32", "cryptbase",
        // 调试/性能
        "dbgcore", "dbghelp", "psapi", "version",
    };
    return kSet;
}

// ---- 视图层构建 ----

void buildViewRecursive(CallNode* src, CallNodeView* dstParent,
                        const BuildViewOptions& opts)
{
    if (!src) return;

    // 1) 决定本层子节点列表（合并 / 不合并）
    struct Bucket {
        uint64_t addr = 0;
        std::string sym;
        std::string module;
        uint32_t hits = 0;
        uint64_t firstSeq = ~0ULL;
        std::vector<CallNode*> sources;   // 折叠到此 bucket 的所有原始 CallNode
    };
    std::vector<Bucket> buckets;
    std::unordered_map<uint64_t, std::size_t> idx;

    for (auto& cu : src->children) {
        CallNode* c = cu.get();
        if (opts.mergeSiblings) {
            auto it = idx.find(c->addr);
            if (it == idx.end()) {
                Bucket b;
                b.addr     = c->addr;
                b.sym      = c->sym;
                b.module   = extractModule(c->sym);
                b.hits     = c->hits ? c->hits : 1;
                b.firstSeq = c->firstSeq;
                b.sources.push_back(c);
                idx[c->addr] = buckets.size();
                buckets.push_back(std::move(b));
            } else {
                Bucket& b = buckets[it->second];
                b.hits   += (c->hits ? c->hits : 1);
                if (c->firstSeq < b.firstSeq) b.firstSeq = c->firstSeq;
                b.sources.push_back(c);
            }
        } else {
            Bucket b;
            b.addr     = c->addr;
            b.sym      = c->sym;
            b.module   = extractModule(c->sym);
            b.hits     = c->hits ? c->hits : 1;
            b.firstSeq = c->firstSeq;
            b.sources.push_back(c);
            buckets.push_back(std::move(b));
        }
    }

    // 2) 对每个 bucket 决定：
    //    - 系统模块且 hideSystemModules=true → 折叠为占位（不再递归子树）
    //    - 否则正常递归
    //    系统模块占位会按"父节点连续聚合"：连续多个 bucket 都是系统模块，
    //    会被合并成 "<system: N calls (mod1, mod2, ...)>" 一个节点。
    auto pushNormal = [&](Bucket& b) {
        if (opts.minHits && b.hits < opts.minHits) return;
        auto v = std::make_unique<CallNodeView>();
        v->addr     = b.addr;
        v->sym      = b.sym;
        v->module   = b.module;
        v->hits     = b.hits;
        v->firstSeq = b.firstSeq;

        // 递归：把所有源节点的子节点合并到一个临时父，再递归
        // 这里采用"逐源递归并把结果挂到 v 下"的方式简单起见，
        // 然后对 v->children 再做一次同 addr 合并。
        for (CallNode* s : b.sources) {
            buildViewRecursive(s, v.get(), opts);
        }

        // 合并 v->children 中同 addr 的项（来自不同源的合并）
        if (opts.mergeSiblings && v->children.size() > 1) {
            std::vector<std::unique_ptr<CallNodeView>> merged;
            std::unordered_map<uint64_t, std::size_t> midx;
            for (auto& cv : v->children) {
                auto it = midx.find(cv->addr);
                if (it == midx.end()) {
                    midx[cv->addr] = merged.size();
                    merged.push_back(std::move(cv));
                } else {
                    auto& tgt = merged[it->second];
                    tgt->hits += cv->hits;
                    if (cv->firstSeq < tgt->firstSeq) tgt->firstSeq = cv->firstSeq;
                    // 把 cv 的孩子搬到 tgt
                    for (auto& gc : cv->children) {
                        tgt->children.push_back(std::move(gc));
                    }
                }
            }
            // 排序：hits 降序
            std::sort(merged.begin(), merged.end(),
                      [](const std::unique_ptr<CallNodeView>& a,
                         const std::unique_ptr<CallNodeView>& b){
                          if (a->hits != b->hits) return a->hits > b->hits;
                          return a->firstSeq < b->firstSeq;
                      });
            v->children = std::move(merged);
        }

        dstParent->children.push_back(std::move(v));
    };

    // 系统模块连续聚合
    auto flushSystem = [&](std::vector<Bucket*>& sysGroup) {
        if (sysGroup.empty()) return;
        if (sysGroup.size() == 1) {
            // 单个系统调用：仍然显示名字（折叠子树即可）
            Bucket& b = *sysGroup.front();
            if (opts.minHits && b.hits < opts.minHits) {
                sysGroup.clear();
                return;
            }
            auto v = std::make_unique<CallNodeView>();
            v->addr     = b.addr;
            v->sym      = b.sym;
            v->module   = b.module;
            v->hits     = b.hits;
            v->firstSeq = b.firstSeq;
            v->isFolded = true;   // 标记：子树被折叠
            char buf[64];
            std::snprintf(buf, sizeof(buf), " [%s, hidden subtree]", b.module.c_str());
            v->sym += buf;
            dstParent->children.push_back(std::move(v));
        } else {
            uint32_t totalHits = 0;
            uint64_t firstSeq  = ~0ULL;
            std::unordered_set<std::string> mods;
            for (auto* b : sysGroup) {
                totalHits += b->hits;
                if (b->firstSeq < firstSeq) firstSeq = b->firstSeq;
                mods.insert(b->module);
            }
            auto v = std::make_unique<CallNodeView>();
            v->addr     = 0;
            v->hits     = totalHits;
            v->firstSeq = firstSeq;
            v->isFolded = true;
            std::string modList;
            std::size_t cnt = 0;
            for (const auto& m : mods) {
                if (cnt++) modList += ", ";
                modList += m;
                if (cnt >= 4) { modList += ", ..."; break; }
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf), "<system: %u calls in %s>",
                          totalHits, modList.c_str());
            v->sym = buf;
            dstParent->children.push_back(std::move(v));
        }
        sysGroup.clear();
    };

    std::vector<Bucket*> sysGroup;
    for (auto& b : buckets) {
        bool isSys = opts.hideSystemModules && !b.module.empty()
                     && CallGraph::isSystemModule(b.module);
        if (isSys) {
            sysGroup.push_back(&b);
        } else {
            flushSystem(sysGroup);
            pushNormal(b);
        }
    }
    flushSystem(sysGroup);
}

}  // namespace

std::unique_ptr<CallNode> CallGraph::build(const std::vector<TraceEvent>& events)
{
    auto root = std::make_unique<CallNode>();
    root->addr = 0;
    root->sym  = "<root>";

    CallNode* cur = root.get();

    for (const auto& ev : events) {
        if (ev.kind == TraceEventKind::Call) {
            auto child       = std::make_unique<CallNode>();
            child->addr      = ev.callee;
            child->sym       = ev.calleeSym.empty()
                                   ? std::string("sub_") + std::to_string(ev.callee)
                                   : ev.calleeSym;
            child->hits      = 1;
            child->firstSeq  = ev.seq;
            // call 之后即将压入返回地址，进入子函数后 sp 会再变；
            // 这里记录 call 发生时的 sp，作为该子函数"对应"的 ret-sp 阈值
            child->spOnEntry = ev.sp;
            child->parent    = cur;

            CallNode* raw    = child.get();
            cur->children.push_back(std::move(child));
            cur = raw;
        } else {
            // Ret：回到 sp >= ev.sp 的最近祖先。
            // 注意 ret 时 sp 已经增加（pop 了返回地址），通常 ret 后的 sp >= call 时的 sp。
            // 因此寻找 spOnEntry < ev.sp 的最近祖先的父节点。
            CallNode* p = cur;
            while (p && p != nullptr && p->parent != nullptr) {
                if (p->spOnEntry >= ev.sp) {
                    // 当前 frame 已经"被 ret 弹出"
                    p = p->parent;
                } else {
                    break;
                }
            }
            cur = p ? p : root.get();
        }
    }

    return root;
}

std::vector<CallPath> CallGraph::findCallers(CallNode* root, uint64_t target)
{
    std::vector<CallPath>  out;
    std::vector<CallNode*> stack;
    if (!root) return out;
    dfsFind(root, target, stack, out);
    return out;
}

void CallGraph::summarize(CallNode* root, std::size_t* nodes, std::size_t* maxDepth)
{
    std::size_t n = 0, d = 0;
    if (root) {
        for (auto& c : root->children) dfsSummary(c.get(), 1, n, d);
    }
    if (nodes)    *nodes    = n;
    if (maxDepth) *maxDepth = d;
}

bool CallGraph::isSystemModule(const std::string& mod)
{
    if (mod.empty()) return false;
    return systemModules().count(mod) != 0;
}

std::unique_ptr<CallNodeView> CallGraph::buildView(CallNode* root,
                                                   const BuildViewOptions& opts)
{
    auto view = std::make_unique<CallNodeView>();
    view->sym = "<root>";
    if (!root) return view;
    buildViewRecursive(root, view.get(), opts);
    return view;
}

}  // namespace x64ai
