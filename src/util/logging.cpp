#include "logging.h"

#include "paths.h"

#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <vector>

namespace x64ai {

namespace {
constexpr const char* kLoggerName      = "x64dbg-ai";
constexpr const char* kAuditLoggerName = "x64dbg-ai-audit";
std::shared_ptr<spdlog::logger> g_logger;
std::shared_ptr<spdlog::logger> g_audit;
}  // namespace

void initLogging() {
    if (g_logger) return;

    auto logFile = pluginLogDir() / "plugin.log";

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logFile.string(), 1024 * 1024 * 4 /*4 MB*/, 5));
    sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());

    g_logger = std::shared_ptr<spdlog::logger>(
        new spdlog::logger(kLoggerName, sinks.begin(), sinks.end()));
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%l] %v");
    g_logger->set_level(spdlog::level::debug);
    g_logger->flush_on(spdlog::level::warn);
    spdlog::register_logger(g_logger);

    // S3-B：写工具审计 logger（独立文件、独立 sink、纯 JSON 一行）
    auto auditFile = pluginLogDir() / "write_audit.log";
    auto auditSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        auditFile.string(), 1024 * 1024 * 4 /*4 MB*/, 10);
    g_audit = std::make_shared<spdlog::logger>(kAuditLoggerName, auditSink);
    // 不加任何前缀：每条消息已是完整 JSON
    g_audit->set_pattern("%v");
    g_audit->set_level(spdlog::level::info);
    g_audit->flush_on(spdlog::level::info);  // 审计必须立刻落盘
    spdlog::register_logger(g_audit);
}

void shutdownLogging() {
    if (g_audit) {
        g_audit->flush();
        spdlog::drop(kAuditLoggerName);
        g_audit.reset();
    }
    if (g_logger) {
        g_logger->flush();
        spdlog::drop(kLoggerName);
        g_logger.reset();
    }
}

std::shared_ptr<spdlog::logger> log() {
    if (!g_logger) initLogging();
    return g_logger;
}

std::shared_ptr<spdlog::logger> auditLog() {
    if (!g_audit) initLogging();
    return g_audit;
}

}  // namespace x64ai
