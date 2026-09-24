// debugger/disasm_context.cpp
#include "debugger/disasm_context.h"

#include <algorithm>
#include <cstdio>
#include <limits>

#include <Windows.h>

#include "bridgemain.h"

#include "util/logging.h"

namespace x64ai {

namespace {

std::string toHexBytes(const unsigned char* p, int n)
{
    std::string s;
    s.reserve(n * 3);
    char buf[4];
    for (int i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", p[i]);
        if (i) s.push_back(' ');
        s.append(buf);
    }
    return s;
}

void captureInstructionRange(DisasmContext& ctx, uint64_t start,
                             uint64_t selectionEnd, uint64_t scanEnd, int maxLines)
{
    ctx.selectionStart = start;
    ctx.selectionEnd = selectionEnd;
    ctx.contextStart = start;

    duint cur = static_cast<duint>(start);
    for (int i = 0; i < maxLines && static_cast<uint64_t>(cur) <= scanEnd; ++i) {
        BASIC_INSTRUCTION_INFO info{};
        DbgDisasmFastAt(cur, &info);
        if (info.size <= 0 || info.size > 16) {
            XAI_LOG_WARN("DbgDisasmFastAt at 0x{:x} returned invalid size {}",
                         static_cast<uint64_t>(cur), info.size);
            break;
        }

        DisasmLine line;
        line.va = static_cast<uint64_t>(cur);
        line.size = info.size;
        line.mnemonic = info.instruction;

        unsigned char raw[16] = {0};
        if (DbgMemRead(cur, raw, static_cast<duint>(info.size))) {
            line.bytesHex = toHexBytes(raw, info.size);
        }

        ctx.lines.push_back(std::move(line));
        ctx.contextEnd = static_cast<uint64_t>(cur) + static_cast<uint64_t>(info.size);
        cur += info.size;
    }
    if (!ctx.lines.empty() && static_cast<uint64_t>(cur) <= scanEnd) {
        XAI_LOG_WARN("disassembly selection truncated at {} instructions", maxLines);
    }
}

}  // namespace

DisasmContext captureCurrentDisasmContext(int maxLines)
{
    DisasmContext ctx;
    ctx.debugging = DbgIsDebugging();
    if (!ctx.debugging) {
        XAI_LOG_DEBUG("captureCurrentDisasmContext: not debugging");
        return ctx;
    }

    SELECTIONDATA sel{};
    if (!GuiSelectionGet(GUI_DISASSEMBLY, &sel)) {
        XAI_LOG_WARN("GuiSelectionGet(GUI_DISASSEMBLY) failed");
        return ctx;
    }
    captureInstructionRange(ctx, static_cast<uint64_t>(sel.start),
                            static_cast<uint64_t>(sel.end),
                            std::numeric_limits<uint64_t>::max(), maxLines);
    return ctx;
}

DisasmContext captureSelectedDisasmContext(int maxLines)
{
    DisasmContext ctx;
    ctx.debugging = DbgIsDebugging();
    if (!ctx.debugging) return ctx;

    SELECTIONDATA sel{};
    if (!GuiSelectionGet(GUI_DISASSEMBLY, &sel)) {
        XAI_LOG_WARN("GuiSelectionGet(GUI_DISASSEMBLY) failed");
        return ctx;
    }

    const uint64_t start = static_cast<uint64_t>(sel.start);
    const uint64_t end = static_cast<uint64_t>(sel.end);
    if (end < start || maxLines <= 0) return ctx;

    captureInstructionRange(ctx, start, end, end, maxLines);
    return ctx;
}

std::string renderDisasmContextMarkdown(const DisasmContext& ctx)
{
    if (!ctx.debugging) {
        return "（当前未处于调试状态，无可分析的反汇编上下文。）";
    }
    if (ctx.lines.empty()) {
        return "（未取到反汇编内容，请先在反汇编窗口选中目标地址。）";
    }

    char hdr[128];
    std::snprintf(hdr, sizeof(hdr),
                  "Disassembly context: 0x%llX - 0x%llX (%zu instructions)\n",
                  static_cast<unsigned long long>(ctx.contextStart),
                  static_cast<unsigned long long>(ctx.contextEnd),
                  ctx.lines.size());

    std::string out;
    out.reserve(ctx.lines.size() * 80 + 64);
    out.append(hdr);
    out.append("```asm\n");

    char addrBuf[24];
    for (const auto& l : ctx.lines) {
        std::snprintf(addrBuf, sizeof(addrBuf), "0x%016llX",
                      static_cast<unsigned long long>(l.va));
        out.append(addrBuf);
        out.append("  ");
        // 字节列定宽 24 字符（最多 16 字节 = 47 字符，截断到 24）
        std::string bytes = l.bytesHex;
        if (bytes.size() > 24) bytes.resize(24);
        out.append(bytes);
        if (bytes.size() < 24) out.append(24 - bytes.size(), ' ');
        out.append("  ");
        out.append(l.mnemonic);
        out.push_back('\n');
    }
    out.append("```\n");
    return out;
}

}  // namespace x64ai
