#include "app/App.h"
#include "util/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  // std::getenv is allowed to return nullptr (e.g. in a non-interactive
  // session). Fall back to "." so std::string concatenation never
  // dereferences a null. We use the error_code overload of
  // create_directories so a permission error on a locked-down machine
  // doesn't abort the process before the logger can even come up.
  const char* base = std::getenv("LOCALAPPDATA");
  std::string logDir = std::string(base ? base : ".") + "\\VMosue\\logs";
  std::error_code ec;
  std::filesystem::create_directories(logDir, ec);
  // ec is ignored on purpose: if we can't make the directory, the
  // rotating file sink will fall back to stderr-only logging.

  vmosue::InitLogger(logDir, "info");
  VMOSUE_LOG_INFO("VMosue starting...");

  vmosue::App app;
  return app.Run();
}
