// debugger/disasm_context.h
//
// 抓取 x64dbg 反汇编窗口当前选中地址附近的反汇编上下文，
// 供 AI 分析使用。所有 Bridge/Dbg API 调用都是同步的、线程安全。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x64ai {

struct DisasmLine {
    uint64_t    va = 0;
    int         size = 0;
    std::string mnemonic;     // BASIC_INSTRUCTION_INFO.instruction（纯文本）
    std::string bytesHex;     // 形如 "48 89 5C 24 08"
};

struct DisasmContext {
    bool                    debugging = false;     // DbgIsDebugging()
    uint64_t                selectionStart = 0;
    uint64_t                selectionEnd   = 0;
    uint64_t                contextStart   = 0;
    uint64_t                contextEnd     = 0;
    std::vector<DisasmLine> lines;
};

// 取当前反汇编窗口选区，向下抓 maxLines 条指令（含选区起点）。
// 若未在调试中，返回 debugging=false，其它字段空。
DisasmContext captureCurrentDisasmContext(int maxLines = 32);

// 把 DisasmContext 渲染为发送给 LLM 的 markdown 文本。
std::string renderDisasmContextMarkdown(const DisasmContext& ctx);

}  // namespace x64ai
