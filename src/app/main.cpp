#include "util/Logger.h"
#include <filesystem>
#include <cstdlib>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  std::string logDir = std::string(std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : ".") + "\\VMosue\\logs";
  std::filesystem::create_directories(logDir);
  vmosue::InitLogger(logDir, "info");
  VMOSUE_LOG_INFO("VMosue v0.1.0 starting...");
  return 0;
}
