// util/hashing.h
//
// 文件 / 缓冲区 SHA-256 计算（OpenSSL EVP）。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace x64ai {

// 计算文件内容 SHA-256，返回小写 hex（64 字符）。失败返回空串。
std::string sha256File(const std::filesystem::path& path);

// 计算字节缓冲 SHA-256，返回小写 hex。
std::string sha256Bytes(std::string_view data);

}  // namespace x64ai
