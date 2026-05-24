// util/secret_store.cpp
#include "util/secret_store.h"

#include <filesystem>
#include <fstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN_DEFINED_LOCALLY
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include "util/config.h"
#include "util/logging.h"
#include "util/paths.h"

#pragma comment(lib, "Crypt32.lib")

namespace x64ai {

namespace fs = std::filesystem;

namespace {

fs::path secretsDir()
{
    auto p = pluginRootDir() / "secrets";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path secretPath(const std::string& name)
{
    return secretsDir() / (name + ".bin");
}

bool writeFileBytes(const fs::path& p, const BYTE* data, size_t n)
{
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    return ofs.good();
}

bool readFileBytes(const fs::path& p, std::vector<BYTE>& out)
{
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) return false;
    out.resize(static_cast<size_t>(sz));
    ifs.seekg(0);
    ifs.read(reinterpret_cast<char*>(out.data()), sz);
    return ifs.good() || ifs.eof();
}

}  // namespace

SecretStore& SecretStore::instance()
{
    static SecretStore inst;
    return inst;
}

bool SecretStore::saveSecret(const std::string& name, const std::string& plaintext)
{
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(plaintext.size());
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"x64dbg-ai-plugin", nullptr, nullptr, nullptr,
                          0 /* CurrentUser */, &out)) {
        XAI_LOG_ERROR("CryptProtectData failed: 0x{:x}", GetLastError());
        return false;
    }
    const auto p = secretPath(name);
    bool ok = writeFileBytes(p, out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!ok) {
        XAI_LOG_ERROR("write secret file failed: {}", p.string());
        return false;
    }
    XAI_LOG_INFO("secret saved: {} ({} bytes encrypted)", name, plaintext.size());
    return true;
}

std::optional<std::string> SecretStore::loadSecret(const std::string& name)
{
    const auto p = secretPath(name);
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;

    std::vector<BYTE> enc;
    if (!readFileBytes(p, enc) || enc.empty()) {
        XAI_LOG_WARN("read secret file failed/empty: {}", p.string());
        return std::nullopt;
    }

    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(enc.size());
    in.pbData = enc.data();

    DATA_BLOB out{};
    LPWSTR descr = nullptr;
    if (!CryptUnprotectData(&in, &descr, nullptr, nullptr, nullptr, 0, &out)) {
        XAI_LOG_ERROR("CryptUnprotectData failed: 0x{:x}", GetLastError());
        return std::nullopt;
    }
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    if (descr) LocalFree(descr);
    LocalFree(out.pbData);
    return plain;
}

bool SecretStore::deleteSecret(const std::string& name)
{
    std::error_code ec;
    return fs::remove(secretPath(name), ec);
}

std::optional<std::string> SecretStore::resolveSecret(const std::string& name,
                                                      const std::string& configFallback)
{
    if (auto s = loadSecret(name); s && !s->empty()) return s;

    if (configFallback.empty()) return std::nullopt;

    // 从 config.json 顶层取 fallback 字段（约定明文字段，开发期使用）
    const auto cfgPath = pluginConfigFile();
    if (!fs::exists(cfgPath)) return std::nullopt;

    try {
        std::ifstream ifs(cfgPath);
        nlohmann::json j;
        ifs >> j;
        if (j.contains(configFallback) && j[configFallback].is_string()) {
            std::string v = j[configFallback].get<std::string>();
            if (!v.empty()) {
                XAI_LOG_WARN("secret '{}' loaded from config.json fallback (PLAINTEXT). "
                             "Use SecretStore::saveSecret to encrypt.", name);
                return v;
            }
        }
    } catch (const std::exception& e) {
        XAI_LOG_WARN("resolveSecret fallback parse error: {}", e.what());
    }
    return std::nullopt;
}

}  // namespace x64ai
