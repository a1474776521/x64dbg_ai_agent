// ai/sse_parser.cpp
#include "ai/sse_parser.h"

namespace x64ai {

void SseParser::reset()
{
    buffer_.clear();
    dataBuffer_.clear();
}

void SseParser::flushEvent()
{
    if (!dataBuffer_.empty() && onData_) {
        onData_(dataBuffer_);
    }
    dataBuffer_.clear();
}

void SseParser::feed(std::string_view chunk)
{
    buffer_.append(chunk.data(), chunk.size());

    // 按行拆分；SSE 规范允许 \n 或 \r\n
    size_t start = 0;
    for (size_t i = 0; i < buffer_.size(); ++i) {
        if (buffer_[i] != '\n') continue;

        size_t lineEnd = i;
        if (lineEnd > start && buffer_[lineEnd - 1] == '\r') {
            --lineEnd;
        }
        std::string_view line(buffer_.data() + start, lineEnd - start);
        start = i + 1;

        if (line.empty()) {
            // 空行 -> event 边界
            flushEvent();
            continue;
        }
        if (line.size() >= 5 && line.substr(0, 5) == "data:") {
            // 去掉 "data:" 后可能存在的一个空格
            size_t off = 5;
            if (off < line.size() && line[off] == ' ') ++off;
            if (!dataBuffer_.empty()) dataBuffer_.push_back('\n');
            dataBuffer_.append(line.substr(off));
        }
        // 其他字段当前忽略（id/event/retry/comment）
    }

    // 保留尾部未完整一行
    if (start > 0) {
        buffer_.erase(0, start);
    }
}

}  // namespace x64ai
