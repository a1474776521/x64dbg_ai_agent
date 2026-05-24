// ai/agent_preset.cpp

#include "ai/agent_preset.h"

#include <algorithm>
#include <regex>

namespace x64ai {

namespace {

template<class T>
T jget(const nlohmann::json& j, const char* key, T fallback)
{
    if (!j.contains(key)) return fallback;
    try { return j.at(key).get<T>(); } catch (...) { return fallback; }
}

}  // namespace

nlohmann::json AgentPreset::toJson() const
{
    nlohmann::json j;
    j["id"]                = id;
    j["name"]              = name;
    j["description"]       = description;
    j["systemPrompt"]      = systemPrompt;
    j["userTemplate"]      = userTemplate;
    j["enabledTools"]      = enabledTools;
    j["maxIter"]           = maxIter;
    j["temperature"]       = temperature;
    j["provider"]          = provider;
    j["model"]             = model;
    j["showInContextMenu"] = showInContextMenu;
    j["readonly"]          = readonly;
    return j;
}

AgentPreset AgentPreset::fromJson(const nlohmann::json& j)
{
    AgentPreset p;
    p.id                = jget<std::string>(j, "id", "");
    p.name              = jget<std::string>(j, "name", "");
    p.description       = jget<std::string>(j, "description", "");
    p.systemPrompt      = jget<std::string>(j, "systemPrompt", "");
    p.userTemplate      = jget<std::string>(j, "userTemplate", "");
    if (j.contains("enabledTools") && j["enabledTools"].is_array()) {
        for (const auto& v : j["enabledTools"]) {
            if (v.is_string()) p.enabledTools.push_back(v.get<std::string>());
        }
    }
    p.maxIter           = std::clamp(jget<int>(j, "maxIter", 20), 1, 50);
    p.temperature       = std::clamp(jget<double>(j, "temperature", 0.2), 0.0, 1.5);
    p.provider          = jget<std::string>(j, "provider", "");
    p.model             = jget<std::string>(j, "model", "");
    p.showInContextMenu = jget<bool>(j, "showInContextMenu", true);
    p.readonly          = jget<bool>(j, "readonly", false);
    return p;
}

std::string expandPresetTemplate(const std::string& tmpl,
                                 const PresetTemplateContext& ctx)
{
    std::string out;
    out.reserve(tmpl.size() + ctx.disasmBlock.size() + 128);
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (i + 1 < tmpl.size() && tmpl[i] == '{' && tmpl[i + 1] == '{') {
            auto end = tmpl.find("}}", i + 2);
            if (end != std::string::npos) {
                std::string key = tmpl.substr(i + 2, end - i - 2);
                // trim
                auto l = key.find_first_not_of(" \t");
                auto r = key.find_last_not_of(" \t");
                if (l != std::string::npos) key = key.substr(l, r - l + 1);
                else                         key.clear();

                bool replaced = true;
                if      (key == "cip")        out += ctx.cipHex;
                else if (key == "module")     out += ctx.moduleName;
                else if (key == "selection")  out += ctx.selectionRange.empty() ? "(none)" : ctx.selectionRange;
                else if (key == "disasm")     out += ctx.disasmBlock;
                else if (key == "user")       out += ctx.userQuery;
                else                          replaced = false;

                if (replaced) { i = end + 2; continue; }
            }
        }
        out += tmpl[i++];
    }
    return out;
}

std::vector<AgentPreset> defaultPresets()
{
    std::vector<AgentPreset> v;

    // 1) 自由 Agent —— 啥都不预设，用户问什么答什么
    {
        AgentPreset p;
        p.id           = "freeform";
        p.name         = "自由 Agent";
        p.description  = "不注入上下文，由你提问，Agent 自主调用工具回答。";
        p.systemPrompt =
            "You are a reverse engineering assistant embedded in x64dbg. "
            "You can call read-only tools to inspect the debuggee. "
            "Prefer concrete VAs over guesses. Cite tool evidence when concluding. "
            "OUTPUT LANGUAGE RULE (highest priority, applies to your final answer / "
            "the `content` field, NOT to your internal reasoning): the user-facing "
            "answer MUST be written in Simplified Chinese (zh-CN). Even if your "
            "chain-of-thought is in English, translate the conclusion into Chinese "
            "before emitting it. Keep code, hex addresses, register names, "
            "instructions and identifiers verbatim (do not translate them).";
        p.userTemplate = "{{user}}";
        // 全工具
        p.maxIter = 20;
        p.showInContextMenu = false;   // 不进右键
        p.readonly = true;
        v.push_back(std::move(p));
    }

    // 2) 分析当前函数
    {
        AgentPreset p;
        p.id           = "analyze-function";
        p.name         = "分析当前函数";
        p.description  = "围绕当前 RIP/EIP 所在函数做整体行为分析。";
        p.systemPrompt =
            "You are a reverse engineering assistant. "
            "Analyze the function around the given VA. "
            "Workflow hint: get_function_range -> get_disasm (cover the whole body) -> "
            "follow xrefs and API calls as needed -> give a structured summary "
            "(purpose, key APIs, control flow, suspicious patterns). "
            "Always reason from concrete bytes/disasm, not guesses. "
            "EVIDENCE RULE: any claim about an address, function body, call relation "
            "or data layout MUST be backed by a tool call you actually made in this "
            "session. If you did NOT read it via a tool, either fetch it or label the "
            "statement explicitly as '推测 (speculative)'. Do NOT invent call edges "
            "between functions you have not disassembled. "
            "CONCRETE INPUT RULE: when the user prompt or initial disasm context "
            "contains a concrete input value (e.g. a register/argument value or a "
            "literal like Mystery1(13)), you MUST trace the algorithm step-by-step "
            "with that value and report the final result, in addition to the generic "
            "algorithmic description. "
            "OUTPUT LANGUAGE RULE (highest priority, applies to your final answer / "
            "the `content` field, NOT to your internal reasoning): the user-facing "
            "answer MUST be written in Simplified Chinese (zh-CN). Even if your "
            "chain-of-thought is in English, translate the conclusion into Chinese "
            "before emitting it. Keep code, hex addresses, register names, "
            "instructions and identifiers verbatim (do not translate them).";
        p.userTemplate =
            "Analyze the function at {{cip}} (module {{module}}).\n"
            "Initial disassembly context:\n```\n{{disasm}}\n```";
        p.enabledTools = {
            "get_disasm","read_memory","read_string","get_registers","list_modules",
            "find_xrefs_to","get_function_range","search_pattern",
            "get_callstack","trace_query","locate_api_callers","rag_search",
            "eval_expression","list_breakpoints","wait_for_event",
            // S3：调试控制 + 写
            "set_breakpoint","remove_breakpoint","step_in","step_over","run_until",
            "run_dbg_command",
            // S4：数据写
            "patch_memory","set_register","write_string",
            // S5：脚本
            "list_scripts","load_script","run_script_file"
        };
        v.push_back(std::move(p));
    }

    // 3) 谁调用了这里
    {
        AgentPreset p;
        p.id           = "who-calls-here";
        p.name         = "谁调用了这里";
        p.description  = "重点排查调用方：xref + 调用栈 + trace + 调用点反汇编。";
        p.systemPrompt =
            "You are a reverse engineering assistant. "
            "The user wants to know WHO calls the given address. "
            "Workflow: find_xrefs_to first; for each caller fetch a few lines of disasm; "
            "if trace events exist, cross-check with trace_query; "
            "if get_callstack is available, include it. "
            "Conclude with a deduplicated list of distinct caller functions. "
            "EVIDENCE RULE: only list callers that find_xrefs_to / get_callstack / "
            "trace_query actually returned. Do NOT invent additional callers based on "
            "guessing or 'this looks like it could be called from X'. If a caller looks "
            "plausible but is not in any tool result, label it '推测' or omit it. "
            "OUTPUT LANGUAGE RULE (highest priority, applies to your final answer / "
            "the `content` field, NOT to your internal reasoning): the user-facing "
            "answer MUST be written in Simplified Chinese (zh-CN). Even if your "
            "chain-of-thought is in English, translate the conclusion into Chinese "
            "before emitting it. Keep code, hex addresses, register names, "
            "instructions and identifiers verbatim (do not translate them).";
        p.userTemplate =
            "Find all callers of {{cip}} (module {{module}}). "
            "Report each caller's function and the relevant disassembly snippet.";
        p.enabledTools = {
            "find_xrefs_to","get_disasm","get_function_range",
            "get_callstack","trace_query","rag_search",
            "eval_expression","list_breakpoints"
        };
        v.push_back(std::move(p));
    }

    // 4) 字符串/API 关联
    {
        AgentPreset p;
        p.id           = "string-api-context";
        p.name         = "字符串与 API 关联";
        p.description  = "围绕当前位置追溯涉及到的字符串、API 调用上下文。";
        p.systemPrompt =
            "You are a reverse engineering assistant focused on string/API context. "
            "Locate nearby string references and API call sites; cluster by purpose "
            "(crypto / net / file / anti-debug / etc.). "
            "EVIDENCE RULE: every string / API mentioned must come from a tool result "
            "in this session (read_string, search_pattern, locate_api_callers, "
            "find_xrefs_to, get_disasm). Do NOT invent strings or API names that you "
            "did not actually observe. If the cluster purpose ('crypto'/'net'/...) is "
            "inferred rather than directly evidenced, mark it as '推测'. "
            "OUTPUT LANGUAGE RULE (highest priority, applies to your final answer / "
            "the `content` field, NOT to your internal reasoning): the user-facing "
            "answer MUST be written in Simplified Chinese (zh-CN). Even if your "
            "chain-of-thought is in English, translate the conclusion into Chinese "
            "before emitting it. Keep code, hex addresses, register names, "
            "instructions and identifiers verbatim (do not translate them).";
        p.userTemplate =
            "Around {{cip}} in {{module}}, find related strings and API usages. "
            "Initial disasm:\n```\n{{disasm}}\n```";
        p.enabledTools = {
            "get_disasm","read_string","read_memory",
            "find_xrefs_to","locate_api_callers","search_pattern","rag_search"
        };
        v.push_back(std::move(p));
    }

    // 5) 解释这条指令（轻量；只用读类工具）
    {
        AgentPreset p;
        p.id           = "explain-here";
        p.name         = "解释此处";
        p.description  = "对当前指令及其上下文做简要解释，少调工具、快速回答。";
        p.systemPrompt =
            "You are a concise reverse engineering tutor. "
            "Briefly explain the instruction at the given VA in context. "
            "Use at most 2-3 tool calls. Keep the answer under 200 words. "
            "OUTPUT LANGUAGE RULE (highest priority, applies to your final answer / "
            "the `content` field, NOT to your internal reasoning): the user-facing "
            "answer MUST be written in Simplified Chinese (zh-CN). Even if your "
            "chain-of-thought is in English, translate the conclusion into Chinese "
            "before emitting it. Keep code, hex addresses, register names, "
            "instructions and identifiers verbatim (do not translate them).";
        p.userTemplate =
            "Explain the instruction at {{cip}} ({{module}}).\nContext:\n```\n{{disasm}}\n```";
        p.enabledTools = {
            "get_disasm","read_memory","read_string","find_xrefs_to","get_registers"
        };
        p.maxIter = 6;
        v.push_back(std::move(p));
    }

    for (auto& p : v) p.readonly = true;
    return v;
}

}  // namespace x64ai
