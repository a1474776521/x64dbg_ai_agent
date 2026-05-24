// ai/sse_parser.h
//
// 极简的 Server-Sent Events 解析器，用于解析 OpenAI 风格 chat/completions
// 流式响应。每接收到一段原始字节，调用 feed() 喂入；解析器在解出完整 event
// 时通过回调返回 data 字段（不含 "data: " 前缀，且已去掉行尾 \n）。
//
// 不处理 multi-line data 之外的字段（id/event/retry），对当前用途已足够。
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace x64ai {

class SseParser {
public:
    using DataCallback = std::function<void(std::string_view data)>;

    explicit SseParser(DataCallback onData) : onData_(std::move(onData)) {}

    void feed(std::string_view chunk);
    void reset();

private:
    void flushEvent();

    std::string  buffer_;       // 行缓冲（跨 chunk 拼接）
    std::string  dataBuffer_;   // 当前 event 的 data: 累积
    DataCallback onData_;
};

}  // namespace x64ai
