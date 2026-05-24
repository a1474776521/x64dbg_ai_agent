// storage/meta_keys.h
//
// SessionStore meta 表使用的标准 key 常量。集中定义避免 magic string 散布。
// 新增字段时仅需在此追加；老库未写入这些键时 getMeta 返回 std::nullopt，
// UI 端按缺省值优雅退化。
#pragma once

namespace x64ai::meta_keys {

// 主模块完整路径（写入时刻）。注意：如果用户换机/移动文件，老库里的值会过时；
// 仅作为辅助识别用途，不要据此做关键路径解析。
inline constexpr const char* kExePath     = "exe_path";

// 主模块文件名（无路径），冗余存一份方便 UI 列表显示，无需每次解析 path。
inline constexpr const char* kExeFilename = "exe_filename";

// 首次为该 SHA 创建 db 的 epoch 秒；setMetaIfAbsent 写入，永不覆盖。
inline constexpr const char* kFirstSeen   = "first_seen";

// 最后一次启动调试此程序的 epoch 秒；每次 onDebugStart 覆盖。
inline constexpr const char* kLastSeen    = "last_seen";

}  // namespace x64ai::meta_keys
