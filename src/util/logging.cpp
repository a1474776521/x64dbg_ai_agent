#include "logging.h"

#include "paths.h"

#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <vector>

namespace x64ai {

namespace {
constexpr const char* kLoggerName = "x64dbg-ai";
std::shared_ptr<spdlog::logger> g_logger;
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
}

void shutdownLogging() {
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

}  // namespace x64ai
