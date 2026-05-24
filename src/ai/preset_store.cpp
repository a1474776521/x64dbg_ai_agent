// ai/preset_store.cpp

#include "ai/preset_store.h"

#include "util/logging.h"
#include "util/paths.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

namespace x64ai {

namespace {

std::filesystem::path presetFilePath()
{
    return pluginRootDir() / "agent_presets.json";
}

}  // namespace

PresetStore& PresetStore::instance()
{
    static PresetStore s;
    return s;
}

void PresetStore::load()
{
    if (loaded_) return;
    reload();
}

void PresetStore::reload()
{
    loaded_ = true;
    presets_.clear();

    const auto path = presetFilePath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        XAI_LOG_INFO("PresetStore: no file at '{}', seeding defaults (schema v{})",
                     path.string(), kPresetSchemaVersion);
        presets_ = defaultPresets();
        save();
        return;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        XAI_LOG_WARN("PresetStore: cannot open '{}', falling back to defaults", path.string());
        presets_ = defaultPresets();
        return;
    }
    std::stringstream ss; ss << ifs.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) {
        presets_ = defaultPresets();
        save();
        return;
    }

    try {
        auto j = nlohmann::json::parse(text);

        // 解析两种格式：
        //   v1（旧）: 顶层 array
        //   v2+（新）: {"schemaVersion": N, "presets": [...]}
        int diskSchema = 1;
        const nlohmann::json* arr = nullptr;
        if (j.is_array()) {
            arr = &j;
        } else if (j.is_object() && j.contains("presets") && j["presets"].is_array()) {
            arr = &j["presets"];
            if (j.contains("schemaVersion") && j["schemaVersion"].is_number_integer()) {
                diskSchema = j["schemaVersion"].get<int>();
            }
        } else {
            XAI_LOG_WARN("PresetStore: file root not recognized, reseed");
            presets_ = defaultPresets();
            save();
            return;
        }

        for (const auto& je : *arr) {
            auto p = AgentPreset::fromJson(je);
            if (p.id.empty() || p.name.empty()) continue;
            presets_.push_back(std::move(p));
        }
        XAI_LOG_INFO("PresetStore: loaded {} preset(s) from '{}' (disk schema v{}, code v{})",
                     presets_.size(), path.string(), diskSchema, kPresetSchemaVersion);

        // 版本迁移：磁盘版本低于代码版本 → 用新 defaults 覆盖所有 readonly 预设，
        // 保留用户自定义（readonly=false）。新增的 readonly 预设也会被追加。
        if (diskSchema < kPresetSchemaVersion) {
            XAI_LOG_INFO("PresetStore: migrating readonly presets from v{} to v{}",
                         diskSchema, kPresetSchemaVersion);
            auto fresh = defaultPresets();

            // 1) 移除所有现有的 readonly 预设
            presets_.erase(
                std::remove_if(presets_.begin(), presets_.end(),
                               [](const AgentPreset& p) { return p.readonly; }),
                presets_.end());

            // 2) 把新 defaults 插到前面（保持出厂在上，user 在下）
            std::vector<AgentPreset> merged;
            merged.reserve(fresh.size() + presets_.size());
            for (auto& p : fresh) merged.push_back(std::move(p));
            for (auto& p : presets_) merged.push_back(std::move(p));
            presets_ = std::move(merged);

            save();
        }
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("PresetStore: parse failed: {}; reseed defaults", e.what());
        presets_ = defaultPresets();
        save();
    }
}

std::optional<AgentPreset> PresetStore::findById(const std::string& id) const
{
    for (const auto& p : presets_) {
        if (p.id == id) return p;
    }
    return std::nullopt;
}

void PresetStore::upsert(const AgentPreset& p)
{
    if (p.id.empty()) {
        XAI_LOG_WARN("PresetStore::upsert: empty id, skipped");
        return;
    }
    for (auto& e : presets_) {
        if (e.id == p.id) { e = p; save(); return; }
    }
    presets_.push_back(p);
    save();
}

bool PresetStore::remove(const std::string& id)
{
    for (auto it = presets_.begin(); it != presets_.end(); ++it) {
        if (it->id == id) {
            presets_.erase(it);
            save();
            return true;
        }
    }
    return false;
}

void PresetStore::resetToDefaults()
{
    presets_ = defaultPresets();
    save();
}

bool PresetStore::save() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : presets_) arr.push_back(p.toJson());

    nlohmann::json j;
    j["schemaVersion"] = kPresetSchemaVersion;
    j["presets"]       = std::move(arr);

    const auto path    = presetFilePath();
    const auto tmpPath = path.string() + ".tmp";
    try {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            XAI_LOG_ERROR("PresetStore::save: cannot open '{}'", tmpPath);
            return false;
        }
        const std::string text = j.dump(2);
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
        ofs.close();

        std::filesystem::rename(tmpPath, path, ec);
        if (ec) {
            // Windows 上 rename 到已存在文件会失败；先删后重试，最多 3 次（应对 AV/Indexer 短暂占用）
            for (int attempt = 0; attempt < 3 && ec; ++attempt) {
                std::error_code ec2;
                std::filesystem::remove(path, ec2);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ec.clear();
                std::filesystem::rename(tmpPath, path, ec);
            }
            if (ec) {
                XAI_LOG_ERROR("PresetStore::save: rename failed: {}", ec.message());
                // 退路：直接覆写目标
                std::ofstream ofs2(path, std::ios::binary | std::ios::trunc);
                if (!ofs2) {
                    XAI_LOG_ERROR("PresetStore::save: fallback overwrite also failed");
                    std::filesystem::remove(tmpPath, ec);
                    return false;
                }
                ofs2.write(text.data(), static_cast<std::streamsize>(text.size()));
                ofs2.close();
                std::filesystem::remove(tmpPath, ec);
                XAI_LOG_WARN("PresetStore::save: used in-place overwrite fallback");
            }
        }
        XAI_LOG_INFO("PresetStore: saved {} preset(s) to '{}'", presets_.size(), path.string());
        return true;
    } catch (const std::exception& e) {
        XAI_LOG_ERROR("PresetStore::save: exception: {}", e.what());
        return false;
    }
}

}  // namespace x64ai
