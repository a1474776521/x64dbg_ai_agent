// ai/tools/dynamic_context_tools.cpp
//
// M4.4c 动态 + 上下文工具集（4 个）：
//   - get_callstack        当前线程调用栈快照（通过 DBGCALLSTACK）
//   - trace_query          查询 TraceRecorder 录到的 call/ret 事件
//   - locate_api_callers   找到指定 API 在主模块里的调用点（薄包 ApiScanner）
//   - rag_search           在当前 session 的 RAG chunks 里检索相关历史分析
//
// 取舍：
//   - trace_query 不做复杂 filter DSL，给固定字段：kind / va_range / limit / since_seq
//     LLM 可以用 limit + since_seq 翻页
//   - locate_api_callers 只跑 IAT xref，不跑 string/pattern；不写 RAG
//   - rag_search 依赖 EmbeddingClient + 当前 ProjectContext

#include "ai/tools/builtin_tools.h"
#include "ai/tools/tool.h"
#include "ai/tools/tool_context.h"
#include "ai/tools/tool_registry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <Windows.h>

#include "bridgemain.h"
#include "_dbgfunctions.h"

#include "ai/embedding_client.h"
#include "locator/api_scanner.h"
#include "storage/session_store.h"
#include "trace/trace_recorder.h"
#include "trace/trace_event.h"

namespace x64ai {

namespace {

bool parseUInt64(const nlohmann::json& v, std::uint64_t& out)
{
    if (v.is_number_unsigned()) { out = v.get<std::uint64_t>(); return true; }
    if (v.is_number_integer()) {
        auto i = v.get<std::int64_t>();
        if (i < 0) return false;
        out = static_cast<std::uint64_t>(i); return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.empty()) return false;
        try {
            std::size_t pos = 0;
            std::uint64_t r;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                r = std::stoull(s.substr(2), &pos, 16);
                if (pos + 2 != s.size()) return false;
            } else {
                r = std::stoull(s, &pos, 0);
                if (pos != s.size()) return false;
            }
            out = r; return true;
        } catch (...) { return false; }
    }
    return false;
}

std::string formatHexVa(std::uint64_t va)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)va);
    return buf;
}

// ====== get_callstack ======

class GetCallStackTool : public ITool {
public:
    std::string name() const override { return "get_callstack"; }
    std::string description() const override
    {
        return "Get the current thread's call stack snapshot at the time of the breakpoint / pause. "
               "Each frame includes the return address (from), the call site target (to), "
               "stack pointer, and a resolved symbol when available. "
               "Frame 0 is the current (innermost) frame; the last entry is the outermost caller.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"max_frames", {
                    {"type", "integer"},
                    {"description", "Max frames returned (1-128). Default 64."},
                    {"minimum", 1},
                    {"maximum", 128},
                }},
            }},
        };
    }
    std::size_t maxResultBytes() const override { return 32 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        auto* fns = DbgFunctions();
        if (!fns || !fns->GetCallStack) {
            r.ok = false; r.error = "DbgFunctions->GetCallStack unavailable";
            return r;
        }
        int maxFrames = 64;
        if (args.contains("max_frames") && args["max_frames"].is_number_integer()) {
            maxFrames = args["max_frames"].get<int>();
        }
        maxFrames = std::clamp(maxFrames, 1, 128);

        DBGCALLSTACK cs{};
        fns->GetCallStack(&cs);
        std::unique_ptr<DBGCALLSTACK, void(*)(DBGCALLSTACK*)> guard(
            &cs, [](DBGCALLSTACK* p) { if (p->entries) BridgeFree(p->entries); });

        nlohmann::json frames = nlohmann::json::array();
        const int n = std::min(cs.total, maxFrames);
        for (int i = 0; i < n; ++i) {
            const auto& e = cs.entries[i];
            char mod[MAX_MODULE_SIZE] = {0};
            DbgGetModuleAt(static_cast<duint>(e.from), mod);
            frames.push_back({
                {"index",  i},
                {"addr",   formatHexVa(static_cast<std::uint64_t>(e.addr))},  // stack 上保存的位置
                {"from",   formatHexVa(static_cast<std::uint64_t>(e.from))},  // 返回地址
                {"to",     formatHexVa(static_cast<std::uint64_t>(e.to))},    // call 目标
                {"module", mod[0] ? mod : ""},
                {"symbol", e.comment[0] ? e.comment : ""},
            });
        }
        r.data = {
            {"total",    cs.total},
            {"returned", n},
            {"frames",   std::move(frames)},
        };
        return r;
    }
};

// ====== trace_query ======

class TraceQueryTool : public ITool {
public:
    std::string name() const override { return "trace_query"; }
    std::string description() const override
    {
        return "Query recorded trace events from the TraceRecorder. "
               "Returns call/ret events captured by the M3.2 trace subsystem. "
               "Filter by event kind (call/ret/any), VA range, and limit. "
               "Returns failure if trace is not active and has no recorded events.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"kind", {
                    {"type", "string"},
                    {"description", "Event kind filter: 'call', 'ret', or 'any'. Default 'any'."},
                    {"enum", {"call", "ret", "any"}},
                }},
                {"target_va", {
                    {"type", "string"},
                    {"description", "If set, only events with callee == target_va (for call) "
                                    "or caller == target_va are returned."},
                }},
                {"since_seq", {
                    {"type", "integer"},
                    {"description", "Return events with seq > since_seq (for paging)."},
                    {"minimum", 0},
                }},
                {"limit", {
                    {"type", "integer"},
                    {"description", "Max events returned (1-256). Default 64."},
                    {"minimum", 1},
                    {"maximum", 256},
                }},
            }},
        };
    }
    std::size_t maxResultBytes() const override { return 48 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& /*ctx*/) override
    {
        ToolResult r;
        auto& rec = TraceRecorder::instance();
        auto events = rec.snapshot();
        if (events.empty()) {
            r.data = {
                {"total",      0},
                {"recording",  rec.isRecording()},
                {"returned",   0},
                {"events",     nlohmann::json::array()},
                {"note",       "no trace events recorded; start trace via the Trace dialog first"},
            };
            return r;
        }

        std::string kind = "any";
        if (args.contains("kind") && args["kind"].is_string()) kind = args["kind"].get<std::string>();
        std::uint64_t targetVa = 0;
        bool hasTarget = false;
        if (args.contains("target_va")) {
            hasTarget = parseUInt64(args["target_va"], targetVa);
        }
        std::uint64_t sinceSeq = 0;
        if (args.contains("since_seq") && args["since_seq"].is_number_integer()) {
            auto v = args["since_seq"].get<std::int64_t>();
            if (v > 0) sinceSeq = static_cast<std::uint64_t>(v);
        }
        int limit = 64;
        if (args.contains("limit") && args["limit"].is_number_integer()) {
            limit = args["limit"].get<int>();
        }
        limit = std::clamp(limit, 1, 256);

        nlohmann::json arr = nlohmann::json::array();
        int matched = 0;
        for (const auto& ev : events) {
            if (ev.seq <= sinceSeq) continue;
            if (kind == "call" && ev.kind != TraceEventKind::Call) continue;
            if (kind == "ret"  && ev.kind != TraceEventKind::Ret)  continue;
            if (hasTarget) {
                bool hit = false;
                if (ev.kind == TraceEventKind::Call && ev.callee == targetVa) hit = true;
                if (ev.caller == targetVa) hit = true;
                if (!hit) continue;
            }
            ++matched;
            if (static_cast<int>(arr.size()) >= limit) continue;
            arr.push_back({
                {"seq",       ev.seq},
                {"kind",      ev.kind == TraceEventKind::Call ? "call" : "ret"},
                {"caller",    formatHexVa(ev.caller)},
                {"callee",    ev.kind == TraceEventKind::Call ? formatHexVa(ev.callee) : ""},
                {"caller_sym", ev.callerSym},
                {"callee_sym", ev.calleeSym},
                {"sp",        formatHexVa(ev.sp)},
                {"ts_ms",     ev.timestampMs},
            });
        }
        r.data = {
            {"total_recorded", static_cast<std::uint64_t>(events.size())},
            {"matched",        matched},
            {"returned",       static_cast<int>(arr.size())},
            {"recording",      rec.isRecording()},
            {"events",         std::move(arr)},
        };
        return r;
    }
};

// ====== locate_api_callers ======

class LocateApiCallersTool : public ITool {
public:
    std::string name() const override { return "locate_api_callers"; }
    std::string description() const override
    {
        return "Find call sites of a specific imported API in the main module. "
               "Wraps the static IAT xref scanner. "
               "Pass the API name (case-insensitive, e.g. 'CreateFileW', 'send', 'recv'). "
               "Returns matched HeuristicHit entries (one per call site).";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"api_name", {{"type", "string"}, {"description", "API name to filter, e.g. 'CreateFileW'."}}},
                {"max_results", {
                    {"type", "integer"},
                    {"description", "Max call sites returned (1-128). Default 32."},
                    {"minimum", 1},
                    {"maximum", 128},
                }},
            }},
            {"required", {"api_name"}},
        };
    }
    std::size_t maxResultBytes() const override { return 32 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.debuggerActive || !DbgIsDebugging()) {
            r.ok = false; r.error = "debugger is not active";
            return r;
        }
        if (!args.contains("api_name") || !args["api_name"].is_string()) {
            r.ok = false; r.error = "invalid 'api_name'";
            return r;
        }
        std::string apiName = args["api_name"].get<std::string>();
        if (apiName.empty()) {
            r.ok = false; r.error = "empty 'api_name'";
            return r;
        }
        int maxResults = 32;
        if (args.contains("max_results") && args["max_results"].is_number_integer()) {
            maxResults = args["max_results"].get<int>();
        }
        maxResults = std::clamp(maxResults, 1, 128);

        // 直接调 ApiScanner，再 client-side 按名过滤
        auto hits = ApiScanner::scan(/*maxXrefsPerApi=*/16);

        // 大小写不敏感、忽略 W/A/Ex 后缀
        auto canonicalize = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            auto endsWith = [&](const std::string& suf) {
                return s.size() > suf.size() &&
                       s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
            };
            while (endsWith("ex") || endsWith("w") || endsWith("a")) {
                s.resize(s.size() - (endsWith("ex") ? 2 : 1));
            }
            return s;
        };
        const std::string needle = canonicalize(apiName);

        nlohmann::json arr = nlohmann::json::array();
        int matched = 0;
        for (const auto& h : hits) {
            if (h.kind != HitKind::Api) continue;
            if (canonicalize(h.label) != needle) continue;
            ++matched;
            if (static_cast<int>(arr.size()) >= maxResults) continue;
            arr.push_back({
                {"va",       formatHexVa(h.va)},
                {"ref_va",   formatHexVa(h.refVa)},
                {"label",    h.label},
                {"category", h.category},
                {"score",    h.score},
                {"evidence", h.evidence},
            });
        }
        r.data = {
            {"api",       apiName},
            {"matched",   matched},
            {"returned",  static_cast<int>(arr.size())},
            {"call_sites", std::move(arr)},
        };
        return r;
    }
};

// ====== rag_search ======

class RagSearchTool : public ITool {
public:
    std::string name() const override { return "rag_search"; }
    std::string description() const override
    {
        return "Search the current project's RAG chunks for content relevant to the query. "
               "Uses cosine-distance vector search over embeddings already stored for this debuggee. "
               "Returns previously captured disassembly snippets, strings, API hits, and notes. "
               "Use this to recall prior analysis instead of re-deriving from scratch.";
    }
    nlohmann::json parametersSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}, {"description", "Natural-language query."}}},
                {"top_k", {
                    {"type", "integer"},
                    {"description", "Number of results returned (1-16). Default 4."},
                    {"minimum", 1},
                    {"maximum", 16},
                }},
            }},
            {"required", {"query"}},
        };
    }
    std::size_t maxResultBytes() const override { return 32 * 1024; }

    ToolResult invoke(const nlohmann::json& args, ToolContext& ctx) override
    {
        ToolResult r;
        if (!ctx.sessionStore) {
            r.ok = false; r.error = "no active session store (open a session first)";
            return r;
        }
        if (!args.contains("query") || !args["query"].is_string()) {
            r.ok = false; r.error = "invalid 'query'";
            return r;
        }
        const std::string query = args["query"].get<std::string>();
        if (query.empty()) {
            r.ok = false; r.error = "empty 'query'";
            return r;
        }
        int topK = 4;
        if (args.contains("top_k") && args["top_k"].is_number_integer()) {
            topK = args["top_k"].get<int>();
        }
        topK = std::clamp(topK, 1, 16);

        auto emb = EmbeddingClient::instance().embed(query);
        if (emb.empty()) {
            r.ok = false; r.error = "embedding failed (check GitHub Models PAT)";
            return r;
        }
        auto* store = reinterpret_cast<SessionStore*>(ctx.sessionStore);
        auto results = store->searchSimilar(emb, topK);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& rc : results) {
            arr.push_back({
                {"id",       rc.chunk.id},
                {"kind",     rc.chunk.kind},
                {"va",       formatHexVa(rc.chunk.va)},
                {"distance", rc.distance},
                {"text",     rc.chunk.text},
            });
        }
        r.data = {
            {"query",   query},
            {"top_k",   topK},
            {"count",   static_cast<int>(arr.size())},
            {"results", std::move(arr)},
        };
        return r;
    }
};

}  // namespace

void registerDynamicAndContextTools(ToolRegistry& reg)
{
    reg.registerTool(std::make_unique<GetCallStackTool>());
    reg.registerTool(std::make_unique<TraceQueryTool>());
    reg.registerTool(std::make_unique<LocateApiCallersTool>());
    reg.registerTool(std::make_unique<RagSearchTool>());
}

}  // namespace x64ai
