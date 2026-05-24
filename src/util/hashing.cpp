// util/hashing.cpp
#include "util/hashing.h"

#include <array>
#include <cstdio>
#include <vector>

#include <openssl/evp.h>

namespace x64ai {

namespace {

std::string toHex(const unsigned char* buf, size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out[i * 2 + 0] = kHex[(buf[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[buf[i] & 0xF];
    }
    return out;
}

}  // namespace

std::string sha256Bytes(std::string_view data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    std::string out;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, data.data(), data.size()) == 1) {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int mdLen = 0;
        if (EVP_DigestFinal_ex(ctx, md, &mdLen) == 1) {
            out = toHex(md, mdLen);
        }
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

std::string sha256File(const std::filesystem::path& path) {
    FILE* f = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&f, path.wstring().c_str(), L"rb") != 0 || !f) return {};
#else
    f = std::fopen(path.string().c_str(), "rb");
    if (!f) return {};
#endif
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { std::fclose(f); return {}; }
    std::string out;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) {
        std::array<unsigned char, 64 * 1024> buf{};
        size_t n = 0;
        bool ok = true;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
            if (EVP_DigestUpdate(ctx, buf.data(), n) != 1) { ok = false; break; }
        }
        if (ok) {
            unsigned char md[EVP_MAX_MD_SIZE];
            unsigned int mdLen = 0;
            if (EVP_DigestFinal_ex(ctx, md, &mdLen) == 1) {
                out = toHex(md, mdLen);
            }
        }
    }
    EVP_MD_CTX_free(ctx);
    std::fclose(f);
    return out;
}

}  // namespace x64ai
