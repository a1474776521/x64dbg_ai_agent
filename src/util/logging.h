// util/logging.h
#pragma once

#include <memory>
#include <spdlog/spdlog.h>

namespace x64ai {

// 初始化全局日志（轮转文件 + Windows 调试输出）
void initLogging();

// 关闭日志
void shutdownLogging();

// 取全局 logger
std::shared_ptr<spdlog::logger> log();

// S3-B：写工具审计日志（独立文件 write_audit.log）。
// 每条 = JSON one-liner（无 spdlog 装饰），便于离线 grep / jq 分析。
std::shared_ptr<spdlog::logger> auditLog();

}  // namespace x64ai

#define XAI_LOG_TRACE(...) ::x64ai::log()->trace(__VA_ARGS__)
#define XAI_LOG_DEBUG(...) ::x64ai::log()->debug(__VA_ARGS__)
#define XAI_LOG_INFO(...)  ::x64ai::log()->info(__VA_ARGS__)
#define XAI_LOG_WARN(...)  ::x64ai::log()->warn(__VA_ARGS__)
#define XAI_LOG_ERROR(...) ::x64ai::log()->error(__VA_ARGS__)
