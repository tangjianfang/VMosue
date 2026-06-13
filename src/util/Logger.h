#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>
#include <vector>

namespace vmosue {

// Initializes the global logger. Safe to call multiple times.
inline void InitLogger(const std::string& logDir, const std::string& level = "info") {
  static bool initialized = false;
  if (initialized) return;
  initialized = true;

  auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

  auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      logDir + "/vmosue.log", 5 * 1024 * 1024, 3);
  file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

  std::vector<spdlog::sink_ptr> sinks{console, file};
  auto logger = std::make_shared<spdlog::logger>("vmosue", sinks.begin(), sinks.end());

  if (level == "trace") logger->set_level(spdlog::level::trace);
  else if (level == "debug") logger->set_level(spdlog::level::debug);
  else if (level == "warn") logger->set_level(spdlog::level::warn);
  else if (level == "error") logger->set_level(spdlog::level::err);
  else logger->set_level(spdlog::level::info);

  spdlog::set_default_logger(logger);
  spdlog::flush_every(std::chrono::seconds(5));
}

#define VMOSUE_LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
#define VMOSUE_LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define VMOSUE_LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define VMOSUE_LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#define VMOSUE_LOG_ERROR(...) spdlog::error(__VA_ARGS__)

}  // namespace vmosue
