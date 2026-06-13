#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "config/Calibration.h"

namespace {

// Each test runs in its own temp directory so parallel test execution
// and the typical `ctest -j` invocation don't race on the same
// profile file. Per-test isolation also keeps a corrupted file from
// one test (RecoversFromCorruption) from poisoning another.
class CalibrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // std::filesystem::temp_directory_path() resolves to the user's
    // temp folder, which the OS guarantees is writable. The unique
    // suffix avoids collisions if a previous run left state behind.
    baseDir_ = std::filesystem::temp_directory_path() /
               ("vmosue_calibration_test_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed));
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
};

}  // namespace

TEST_F(CalibrationTest, SaveLoadRoundTrip) {
  vmosue::Calibration c;
  c.SetBaseDir(baseDir_);

  vmosue::CalibrationParams p;
  p.pinchThreshold = 0.05f;
  p.airClickZThreshold = 0.03f;
  p.scaleX = 1.25f;
  p.scaleY = 0.875f;
  p.offsetX = -10.0f;
  p.offsetY = 12.5f;

  auto saved = c.Save("test_user", p);
  ASSERT_TRUE(saved.isOk()) << saved.error();

  auto loaded = c.Load("test_user");
  ASSERT_TRUE(loaded.isOk()) << loaded.error();
  EXPECT_FLOAT_EQ(loaded.value().pinchThreshold, 0.05f);
  EXPECT_FLOAT_EQ(loaded.value().airClickZThreshold, 0.03f);
  EXPECT_FLOAT_EQ(loaded.value().scaleX, 1.25f);
  EXPECT_FLOAT_EQ(loaded.value().scaleY, 0.875f);
  EXPECT_FLOAT_EQ(loaded.value().offsetX, -10.0f);
  EXPECT_FLOAT_EQ(loaded.value().offsetY, 12.5f);
}

TEST_F(CalibrationTest, RecoversFromCorruption) {
  vmosue::Calibration c;
  c.SetBaseDir(baseDir_);

  // Hand-craft a malformed JSON file where Load() expects a profile.
  const auto path = c.ProfilePath("broken_user");
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out.is_open());
  out << "{ this is not valid json";
  out.close();

  auto loaded = c.Load("broken_user");
  EXPECT_FALSE(loaded.isOk());
  EXPECT_FALSE(loaded.error().empty());
}

TEST_F(CalibrationTest, LoadMissingFileReturnsErr) {
  vmosue::Calibration c;
  c.SetBaseDir(baseDir_);

  auto loaded = c.Load("does_not_exist");
  EXPECT_FALSE(loaded.isOk());
  EXPECT_FALSE(loaded.error().empty());
}

TEST_F(CalibrationTest, DefaultBaseDirUsesLocalAppData) {
  // Construction with no override should resolve under %LOCALAPPDATA%
  // (or "." if the env var is missing). We don't want to assert the
  // exact path because CI runners may not have LOCALAPPDATA, but we
  // do want the method to be non-throwing and the path non-empty.
  vmosue::Calibration c;
  auto base = c.BaseDir();
  EXPECT_FALSE(base.empty());
}

TEST_F(CalibrationTest, RunInteractiveIsNotImplemented) {
  // v0.2: RunInteractive is a UI flow that depends on a live hand
  // pipeline, which lives in a later task. It must return Err so
  // callers can detect the missing UI and fall back to defaults.
  vmosue::Calibration c;
  c.SetBaseDir(baseDir_);

  auto result = c.RunInteractive();
  EXPECT_FALSE(result.isOk());
  EXPECT_FALSE(result.error().empty());
}

TEST_F(CalibrationTest, RejectsPathTraversalProfileName) {
  vmosue::Calibration c;
  c.SetBaseDir(baseDir_);

  // "../escape" would write outside baseDir_ if accepted. The
  // implementation must reject it and leave the base dir unchanged.
  auto saved = c.Save("../escape", vmosue::CalibrationParams{});
  EXPECT_FALSE(saved.isOk());

  // baseDir_ must not have been used as a parent for the bad name.
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(baseDir_, ec)) {
    EXPECT_NE(entry.path().filename().string(), "escape.json");
  }
}
