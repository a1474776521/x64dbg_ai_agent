// util/encoding.h
//
// 字符编码转换工具（S3 中文路径根治）。
//
// 设计原则：
//   - 插件内部一律 UTF-8（std::string）。
//   - 与 Windows / x64dbg SDK 之间的边界做转码：
//       SDK 给我们的 char* (PLUG_CB_*.szFileName 等) 在中文系统上是 ACP（GBK），
//       必须先转 UTF-8 再交给 std::filesystem / sqlite / spdlog。
//   - std::filesystem::path 一律用 fsPathFromUtf8() 构造，不要直接传 std::string，
//     因为 MSVC 的 path(string) 会按 ACP 解释，中文路径会乱码。
//   - sqlite3_open 接 UTF-8 路径（API 文档明确说），所以 path.string() 必须先转 UTF-8。
//
// 命名：utf8/ansi/wide = std::string(UTF-8) / std::string(ACP) / std::wstring(UTF-16)。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace x64ai {

// Windows ACP (一般 = GBK) -> UTF-8。空串安全。
std::string ansiToUtf8(std::string_view ansi);

// UTF-8 -> Windows ACP。仅在必须回传 ANSI API 时用（尽量避免）。
std::string utf8ToAnsi(std::string_view utf8);

// std::wstring (UTF-16) <-> UTF-8。空串安全。
std::string  wideToUtf8(std::wstring_view wide);
std::wstring utf8ToWide(std::string_view utf8);

// 用 UTF-8 字符串构造 fs::path（实际走 wstring 通路），避免 MSVC 把 string 当 ACP。
std::filesystem::path fsPathFromUtf8(std::string_view utf8);

// 把 fs::path 序列化成 UTF-8（path.wstring() -> UTF-8），用于喂 sqlite/spdlog/日志输出。
std::string fsPathToUtf8(const std::filesystem::path& p);

// 启发式判断 utf8 字符串是不是合法 UTF-8（false 则可能是 ACP 误存）。
bool isValidUtf8(std::string_view s);

}  // namespace x64ai
