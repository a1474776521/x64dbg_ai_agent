// util/encoding.cpp
#include "util/encoding.h"

#include <Windows.h>

namespace x64ai {

namespace {

std::wstring multibyteToWide(UINT codePage, std::string_view src)
{
    if (src.empty()) return {};
    const int srcLen = static_cast<int>(src.size());
    const int n = ::MultiByteToWideChar(codePage, 0, src.data(), srcLen, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    const int written = ::MultiByteToWideChar(codePage, 0, src.data(), srcLen,
                                              out.data(), n);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string wideToMultibyte(UINT codePage, std::wstring_view src)
{
    if (src.empty()) return {};
    const int srcLen = static_cast<int>(src.size());
    const int n = ::WideCharToMultiByte(codePage, 0, src.data(), srcLen,
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    const int written = ::WideCharToMultiByte(codePage, 0, src.data(), srcLen,
                                              out.data(), n, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

}  // namespace

std::string ansiToUtf8(std::string_view ansi)
{
    auto w = multibyteToWide(CP_ACP, ansi);
    return wideToMultibyte(CP_UTF8, w);
}

std::string utf8ToAnsi(std::string_view utf8)
{
    auto w = multibyteToWide(CP_UTF8, utf8);
    return wideToMultibyte(CP_ACP, w);
}

std::string wideToUtf8(std::wstring_view wide)
{
    return wideToMultibyte(CP_UTF8, wide);
}

std::wstring utf8ToWide(std::string_view utf8)
{
    return multibyteToWide(CP_UTF8, utf8);
}

std::filesystem::path fsPathFromUtf8(std::string_view utf8)
{
    // 关键：用 wstring 构造，绕开 MSVC 把 std::string 当 ACP 的坑。
    return std::filesystem::path(utf8ToWide(utf8));
}

std::string fsPathToUtf8(const std::filesystem::path& p)
{
    return wideToUtf8(p.wstring());
}

bool isValidUtf8(std::string_view s)
{
    // 用 MultiByteToWideChar + MB_ERR_INVALID_CHARS 严格校验。
    if (s.empty()) return true;
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    return n > 0;
}

}  // namespace x64ai
