# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

VMosue 是 Windows 10/11 原生手势控制鼠标应用：通过摄像头识别手部动作驱动光标移动和点击，无需手套、控制器或特殊硬件。C++20 主体 + Python MediaPipe 子进程，C++ 端通过 stdin/stdout JSON 协议与检测器通信。v1.0.0 已发布，`main` 分支持续迭代（见 `CHANGELOG.md [Unreleased]` 与 `docs/ROADMAP.md`）。

## 构建

CMake + Ninja + vcpkg。vcpkg 依赖（`vcpkg.json`）：`spdlog`、`nlohmann-json`、`boost-lockfree`、`gtest`。

**务必用 `build-ninja.bat`（或 `build-cmake.bat`）而不是裸 `cmake --build`**：脚本会先 `call vcvars64.bat`，绕过裸调用链接失败的常见坑。

```powershell
# 一次性引导（克隆并 bootstrap vcpkg 到 %USERPROFILE%\vcpkg）
.\scripts\bootstrap.ps1

# 准备运行时资源（必跑：模型 + Python 依赖，否则 vmosue.exe 无法启动）
.\scripts\prepare-resources.ps1

# 配置 + 编译（bat 内置 vcvars）
cmd //c ".\build-ninja.bat"

# 等价的裸命令（手动挡，需先 source vcvars64）
cmake -G Ninja -B build -S . `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DCMAKE_BUILD_TYPE=Release `
  -DVMOSUE_FETCH_MEDIAPIPE=OFF        # 离线机器：跳过 MediaPipe FetchContent
cmake --build build --config Release
```

**关键编译开关**：
- `VMOSUE_BUILD_TESTS=ON`（默认）：构建 `vmosue_tests`
- `VMOSUE_FETCH_MEDIAPIPE=OFF`：跳过 MediaPipe FetchContent，`vmosue_inference` 退化为 stub；offline 机器必加
- `VMOSUE_COVERAGE=ON`：GCC/Clang 加 `--coverage`；MSVC 无对应会被忽略，改用 OpenCppCoverage

**ATL 要求**：`src/capture/CameraCapture.h` 依赖 `<atlbase.h>`，构建 `vmosue.exe` 需要 VS Installer 里勾选 **C++ ATL for latest v143 build tools**。若 CMake 选到无 ATL 的 BuildTools 实例，加 `-DCMAKE_GENERATOR_INSTANCE` 指向含 ATL 的 VS 实例（详见 `docs/build-notes.md`）。

## 测试

GoogleTest + `gtest_discover_tests`（每个 `TEST(...)` 注册为独立 ctest 用例）。

```powershell
# 全量
ctest --test-dir build -C Release --output-on-failure

# 按 gtest test name 过滤（单测 / 子集）
ctest --test-dir build -R "ActionMap.Click_Pinch" --output-on-failure
ctest --test-dir build -R "ActionMap.*" --output-on-failure

# 直接跑二进制（gtest 原生过滤）
.\build\bin\vmosue_tests.exe --gtest_filter="*Pinch*"

# 主驱动脚本（parsecheck + build + test + installer 分阶段）
.\run_all.bat                # 默认 all
.\run_all.bat build          # 只做 cmake configure + build
.\run_all.bat test           # 只跑 vmosue_tests
.\run_all.bat parsecheck     # 离线 fallback：每个 TU 走 cl /EHa 解析检查

# Coverage（GCC/Clang only）
bash scripts/run-coverage.sh
```

**回归约定**：任何手势阈值或新 action 改动，**先跑 `tests/fixtures/actions/` 下的合成 landmark fixture**，确认无 cross-talk 回归：
```powershell
ctest --test-dir build -C Release -R ActionMap --output-on-failure
```

## 架构概览

### 三线程管线

`src/app/App.cpp` 编排三条 worker 线程，线程间通过 `boost::lockfree::spsc_queue` 单生产者-单消费者环形缓冲通信（无锁、无信号量）：

```
T1 captureLoop        : CameraCapture           -> frameQ_       (cap=2)
T2 inferenceLoop      : frameQ_ -> HandDetector -> LandmarkSmoother -> landmarkQ_   (cap=4)
T3 stateMachineLoop   : landmarkQ_ -> GestureStateMachine -> InputInjector
```

所有跨线程退出由单一原子 `running_` 标志驱动：`App::Shutdown()` 翻转标志并 join。worker 任何未捕获异常走 `App::NotifyThreadError()`：置 `threadError_`、发 `WM_CLOSE` 唤醒主消息循环、停运行。详见 `src/app/App.h`。

### 模块边界（`src/<module>/`）

| Module        | 职责                                                                                              |
|---------------|---------------------------------------------------------------------------------------------------|
| `app/`        | `wWinMain` 入口、`App` 主控（线程编排 + 窗口/托盘生命周期）、`Watchdog` 守护线程                     |
| `capture/`    | Windows Media Foundation 摄像头采集 + NV12→BGRA；`Frame` 类型定义在此                              |
| `inference/`  | `HandDetector`（C++ ↔ Python IPC 桥）+ `LandmarkSmoother`（1-Euro）                                |
| `gesture/`    | `GestureStateMachine` 聚合 `CursorController` / `ClickDetector` / `AirClickDetector` / `ScrollDetector` / `PauseDetector` |
| `input/`      | `InputInjector`：Win32 `SendInput` 包装                                                            |
| `ui/`         | Direct2D 叠加层、托盘图标、Settings/Debug/Tutorial 窗口                                             |
| `config/`     | `Config`（`%LOCALAPPDATA%\VMosue\config.json`，原子写）+ `Calibration`                             |
| `platform/`   | 多显示器 `DisplayInfo`、全局热键 `Hotkey`、自启动 `AutoStart`                                      |
| `util/`       | `Logger`（spdlog + 轮转）、`ProfileGuard`（P50/P95）、`Adaptive`（v0.5 滚动观测）、`OneEuroFilter` / `Kalman1D` 等纯头算法 |

每模块自带 `CMakeLists.txt`，产物为 `vmosue_<module>` 静态库；`vmosue` 可执行文件在 `src/app/CMakeLists.txt` 链接所有库 + `vmosue_runtime_data`（资源同步自定义 target）。

### C++ ↔ Python IPC

`HandDetector` 不在进程内跑 MediaPipe，而是 spawn `scripts/hand_detector_server.py` 子进程。协议（详见脚本顶部 docstring）：

- **C++ → Python**：`{"width","height","length"}\n` + `length` 字节 BGRA 像素（BGR + alpha，row-major，无 padding）
- **Python → C++**：`{"hand_count","image_width","image_height","hands":[{handedness, score, landmarks, world_landmarks}, ...]}\n`

`world_landmarks` 是 metric 坐标，`AirClickDetector` 读 `world[8].z`（食指尖深度）做"推手靠近镜头"右击判定——**若 world landmarks 缺失，右击手势在生产中永远不会触发**（已在 1.0.0 修复过一次）。

子进程崩溃由 `HandDetector::TryRespawn()` 自动恢复，单次死链最多重试 3 次（`HandDetector.h::kMaxRespawns`）。Python 端每帧包了 try/except，单帧错误降级为 "zero hands" 而不是让子进程崩溃。

### v0.5 Adaptive 体系

`util/Adaptive.h` 集中"无硬编码参数"原则：所有 environment-/signal-derived 调参（phantom-hand 过滤分数 gap、叠加层颜色百分位、渲染 cadence、1-Euro 噪声底/运动幅度、pinch/scroll/click 阈值）由 `SignalObserver` 采样、`AdaptiveController` 推导。前 `kColdStartFrames` 帧退到 v0.4 常量并线性混合 ~2 秒。详见 `docs/superpowers/specs/2026-06-17-adaptive-parameters.md`。

### 性能观测

Release 构建自动记录 P50/P95 latency（`lat_capture` / `lat_ipc_rtt` / `lat_gesture`），每秒一行写入 `%LOCALAPPDATA%\VMosue\logs\`。若 `ipc_rtt P95 > 33 ms`，则 ROADMAP D2/D3（shared-memory IPC / frame downscale / SIMD）解锁为下一优先项——这是判定是否做的硬数据，不是凭直觉。完整解读见 `docs/build-notes.md → "Measuring per-frame latency"`。

### 状态机与 Action

`GestureStateMachine::OnLandmarks(...)` 消费 `vector<HandLandmarks>`，产出 `ActionSet`（光标 dx/dy、按钮事件、滚轮 delta、safeRelease），`InputInjector` 翻译成 `SendInput`。`GlobalState { Active, Paused, EmergencyStopped }` 通过 `std::atomic` 共享；Pause/EmergencyStop 会停止消费 actions 直到 Resume。

### 测试结构

- `tests/unit/` — 19 个 GoogleTest 文件，无硬件依赖的纯逻辑
- `tests/integration/test_pipeline_e2e.cpp` + `test_action_map.cpp` — 端到端 + per-action fixture
- `tests/python/test_hand_detector_server.py` — Python 协议测试
- `tests/fixtures/sample_landmarks.json` + `tests/fixtures/actions/*.json` — 合成 landmark 序列（绝对路径通过 `VMOSUE_TEST_FIXTURES_DIR` 编译宏注入测试二进制，见 `tests/CMakeLists.txt`）

集成测试中 `Watchdog.cpp` 被直接编译进 `vmosue_tests`（注释解释了原因：可执行文件不能链接可执行文件，只能重复编译同源）。

## 易踩坑

1. **vcvars**：手动 `cmake --build` 在 PowerShell 直接调用通常 link 失败——必须先 `call vcvars64.bat`。`build-ninja.bat` / `build-cmake.bat` 已封装。
2. **MediaPipe stub**：`src/inference/HandDetector.cpp` 中的 `HandLandmarkerWrapper` 是 deliberate no-op stub（构建环境无法 fetch MediaPipe）。当前真实检测走 Python 子进程，不是 in-process MediaPipe。
3. **运行时数据同步**：`scripts/` 与 `resources/` 必须在二进制旁边（`build/bin/scripts/`、`build/bin/resources/`）。根 `CMakeLists.txt` 的 `add_custom_command` 每次构建同步这两个目录；新加的脚本/资源文件会被自动跟进，但**新加的二进制文件必须列在 `add_custom_command(... DEPENDS ...)` 里**才能触发 stamp 重算。
4. **worker 线程异常**：抛异常未捕获会 std::terminate。新增 worker 必须复制 `captureLoop` 的 try/catch + `NotifyThreadError()` 模式。
5. **配置 / calibration 写入**：`Config::Save` + `Calibration::Save` 用 temp-file + rename 原子化。直接写会被崩溃打断而截断现有文件。
6. **HandLandmarks::world 数组**：Python 子进程必须填充（metric 坐标）。改 IPC 协议时若忘了 C++ 解析端，right-click 手势会在生产中静默失效。

## 文档地图

| 路径 | 内容 |
|------|------|
| `README.md` | 项目介绍、安装、构建命令 |
| `docs/ROADMAP.md` | 后 1.0 backlog（A/B/C/D 分组；D2/D3 待性能数据解锁） |
| `docs/build-notes.md` | 构建踩坑（ATL、网络、FetchContent）、latency 日志解读 |
| `docs/user/` | 用户手册（quickstart / gestures / troubleshooting / tutorial） |
| `docs/user/gesture-action-map.md` | 所有 action 的权威映射（landmark indices、阈值、仲裁） |
| `docs/superpowers/specs/` | 设计 spec |
| `docs/superpowers/plans/` | 实施计划 |
| `docs/acceptance/test-protocol.md` | v1.0 手动验收测试协议 |
| `CHANGELOG.md` | 变更记录（[Unreleased] + 1.0.0 release notes） |

## 常用改动落点

- **改手势阈值**：`src/gesture/GestureStateMachine.cpp` + 对应 detector → 跑 `ActionMap.*` 回归
- **加新 OS 输入**：扩 `ActionSet`（`src/gesture/GestureStateMachine.h`）→ `src/input/InputInjector.cpp` → 加 fixture 走 `test_action_map.cpp`
- **改 IPC 协议**：同时改 `scripts/hand_detector_server.py` 与 `src/inference/HandDetector.cpp` 的 metadata 解析；`world_landmarks` 不可丢
- **加新 UI 字符串**：往 `resources/i18n/` 放新 JSON；`src/util/I18n.cpp` 启动时加载
- **改坐标映射**：**必须**保持自拍镜像公式 `screenX = virtX + (1 - nx) * (W - 1)` 在四处组件完全一致：`CursorController`、`OverlayGeometry::LandmarkToScreen`、DebugWindow 预览位图 `D2D1::Matrix3x2F::Scale(-1, 1)`、DebugWindow 预览骨架点。改完跑 `OverlayGeometry.MatchesCursorController*` 与 `CursorControllerTest.*` 回归 —— 这两组测试是用户投诉"完全不匹配"的回归保护。
