#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "config/Config.h"

namespace {

// Each test runs in its own temp directory so parallel test execution
// and the typical `ctest -j` invocation don't race on the same
// config file. Per-test isolation also keeps a corrupted file from
// one test (RecoversFromCorruption) from poisoning another.
class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // std::filesystem::temp_directory_path() resolves to the user's
    // temp folder, which the OS guarantees is writable. The unique
    // suffix avoids collisions if a previous run left state behind.
    baseDir_ = std::filesystem::temp_directory_path() /
               ("vmosue_config_test_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::error_code ec;
    std::filesystem::remove_all(baseDir_, ec);
    std::filesystem::create_directories(baseDir_, ec);
    ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(baseDir_, ec);
  }

  std::filesystem::path baseDir_;
  std::filesystem::path ConfigFile() const {
    return baseDir_ / "config.json";
  }
};

}  // namespace

TEST_F(ConfigTest, SaveLoadRoundTrip) {
  // The Config singleton has process-wide state. Reset it to a fresh
  // config path so we don't touch the real %LOCALAPPDATA%\VMosue file.
  auto& c = vmosue::Config::Get();
  c.SetConfigPath(ConfigFile());

  // Mutate fields away from defaults to prove the round trip actually
  // serializes and deserializes values rather than returning defaults.
  c.Mutable().activeProfile = "power_user";
  c.Mutable().cameraIndex = 2;
  c.Mutable().performanceMode = "performance";
  c.Mutable().autoStart = true;
  c.Mutable().showTutorialOnLaunch = false;
  c.Mutable().logLevel = "debug";

  auto saved = c.Save();
  ASSERT_TRUE(saved.isOk()) << saved.error();
  ASSERT_TRUE(std::filesystem::exists(ConfigFile()))
      << "Config file was not created at " << ConfigFile();

  // Reset to defaults in-memory so we can prove Load() reads from
  // disk rather than from the prior in-memory state.
  c.Mutable() = vmosue::AppConfig{};

  auto loaded = c.Load();
  ASSERT_TRUE(loaded.isOk()) << loaded.error();
  EXPECT_EQ(c.Data().activeProfile, std::string("power_user"));
  EXPECT_EQ(c.Data().cameraIndex, 2);
  EXPECT_EQ(c.Data().performanceMode, std::string("performance"));
  EXPECT_TRUE(c.Data().autoStart);
  EXPECT_FALSE(c.Data().showTutorialOnLaunch);
  EXPECT_EQ(c.Data().logLevel, std::string("debug"));
}

TEST_F(ConfigTest, RecoversFromCorruption) {
  auto& c = vmosue::Config::Get();
  c.SetConfigPath(ConfigFile());

  // Hand-craft a malformed JSON file at the expected location. After
  // Load() fails the in-memory AppConfig must remain at defaults.
  std::ofstream out(ConfigFile(), std::ios::binary);
  ASSERT_TRUE(out.is_open());
  out << "{ this is not valid json";
  out.close();

  auto loaded = c.Load();
  EXPECT_FALSE(loaded.isOk());
  EXPECT_FALSE(loaded.error().empty());

  // Singleton must have fallen back to defaults.
  EXPECT_EQ(c.Data().activeProfile, std::string("default"));
  EXPECT_EQ(c.Data().cameraIndex, 0);
  EXPECT_EQ(c.Data().performanceMode, std::string("balanced"));
  EXPECT_FALSE(c.Data().autoStart);
  EXPECT_TRUE(c.Data().showTutorialOnLaunch);
  EXPECT_EQ(c.Data().logLevel, std::string("info"));
}

TEST_F(ConfigTest, DefaultsAreCorrect) {
  // A fresh Config (no Load/Save) should report all defaults.
  // We can't construct a Config directly (private ctor) but we can
  // hit a path that doesn't exist, fall back to defaults, and verify.
  auto& c = vmosue::Config::Get();
  c.SetConfigPath(ConfigFile());

  // Path exists in dir but file does not: Load returns Err, data_ is
  // untouched (still at the AppConfig in-class defaults from ctor).
  auto loaded = c.Load();
  EXPECT_FALSE(loaded.isOk());
  EXPECT_EQ(c.Data().activeProfile, std::string("default"));
  EXPECT_EQ(c.Data().cameraIndex, 0);
  EXPECT_EQ(c.Data().performanceMode, std::string("balanced"));
  EXPECT_FALSE(c.Data().autoStart);
  EXPECT_TRUE(c.Data().showTutorialOnLaunch);
  EXPECT_EQ(c.Data().logLevel, std::string("info"));
}

TEST_F(ConfigTest, AtomicWriteCleansUpTempFile) {
  auto& c = vmosue::Config::Get();
  c.SetConfigPath(ConfigFile());

  auto saved = c.Save();
  ASSERT_TRUE(saved.isOk()) << saved.error();

  // On a successful save, the .tmp sidecar should not be left behind.
  const auto tmpPath = ConfigFile().string() + ".tmp";
  EXPECT_FALSE(std::filesystem::exists(tmpPath))
      << "Atomic write left a stale temp file: " << tmpPath;
  EXPECT_TRUE(std::filesystem::exists(ConfigFile()));
}

TEST_F(ConfigTest, LoadMissingFileReturnsErr) {
  auto& c = vmosue::Config::Get();
  c.SetConfigPath(ConfigFile());

  // The directory exists (SetUp created it) but no config.json has
  // been written. Load must return Err so callers know to fall back
  // to defaults or call Save().
  auto loaded = c.Load();
  EXPECT_FALSE(loaded.isOk());
  EXPECT_FALSE(loaded.error().empty());
}

TEST_F(ConfigTest, DefaultConfigPathUsesLocalAppData) {
  // With no override, Config::ConfigPath() must resolve to a path
  // under %LOCALAPPDATA%\VMosue\config.json (or "." if the env var
  // is missing). We don't assert the exact path because CI runners
  // may not have LOCALAPPDATA, but we do want the path to be
  // non-empty and end with config.json.
  auto& c = vmosue::Config::Get();
  // Reset to default by passing an empty path.
  c.SetConfigPath(std::filesystem::path{});
  auto path = c.ConfigPath();
  EXPECT_FALSE(path.empty());
  EXPECT_EQ(path.filename().string(), std::string("config.json"));
}