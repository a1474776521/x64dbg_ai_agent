// util/paths.h
//
// 集中管理插件的本地路径：配置目录、日志目录、会话存储目录、
// GitHub Copilot 鉴权 token 目录等。
#pragma once

#include <filesystem>
#include <string>

namespace x64ai {

// %APPDATA%/x64dbg-ai-plugin/
std::filesystem::path pluginRootDir();

// %APPDATA%/x64dbg-ai-plugin/config.json
std::filesystem::path pluginConfigFile();

// %APPDATA%/x64dbg-ai-plugin/logs/
std::filesystem::path pluginLogDir();

// %APPDATA%/x64dbg-ai-plugin/projects/
std::filesystem::path pluginProjectsDir();

// %APPDATA%/x64dbg-ai-plugin/projects/<sha256>.db
std::filesystem::path projectDbPath(const std::string& sha256Hex);

// %USERPROFILE%/.config/github-copilot/  (与 VS Code Copilot / gh / OpenCode 共享)
std::filesystem::path copilotAuthDir();

}  // namespace x64ai
