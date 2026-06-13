# VMosue 设计文档

**项目名**：VMosue（Vision Mouse）
**目标**：Windows 10/11 x64 原生手势控制鼠标 App
**文档日期**：2026-06-13
**目标版本**：MVP v1.0（可公开发布）

---

## 0. 背景与目标

VMosue 是一款 Windows 本地手势控制鼠标应用。用户通过单摄像头与手势即可控制鼠标移动、点击、拖拽、滚动、暂停/恢复，目标是替代或补充传统鼠标，特别适用于：

- 无鼠标或鼠标损坏的临时替代
- 多媒体播放、PPT 演示、阅读等远距离场景
- 残障辅助（手部可活动但不便持握物理设备）

**核心约束**：

- 仅 Windows 10/11 x64，本地控制，不做远程
- P95 端到端延迟 < 60ms
- 误触发率 ≤ 1%
- CPU 占用 < 25%
- 内存占用 < 300MB
- 开源 Apache-2.0

---

## 1. 总体架构

### 1.1 进程与线程模型

**单进程 + 6 个核心线程**：

| 线程 | 名称 | 职责 | 优先级 | 频率 |
|---|---|---|---|---|
| T1 | CaptureThread | Media Foundation 摄像头采集 → 帧队列 | TIME_CRITICAL | 30-60 fps |
| T2 | InferenceThread | MediaPipe Hands 推理 → 手部关键点队列 | HIGH | 30 fps |
| T3 | StateMachineThread | 消费关键点 → 状态机 → 动作命令队列 | ABOVE_NORMAL | 30-60 Hz |
| T4 | InjectorThread | SendInput 鼠标/键盘注入 | HIGH | 60-120 Hz |
| T5 | UIThread | Win32 消息循环（托盘+设置面板） | NORMAL | 事件驱动 |
| T6 | RenderThread | Direct2D Overlay 渲染 | NORMAL | 60 fps |

**约束**：
- 帧队列使用 `boost::lockfree::spsc_queue`（无锁 SPSC），容量 2（只保留最新帧）
- 关键点队列容量 4
- 动作命令队列容量 16
- 每个线程入口有 `try/catch` + Watchdog 监控
- 线程间无锁；SPSC 用于 1:1 通道，MPSC 用于 1:N 通道

### 1.2 顶层数据流

```
Camera ──► [CaptureThread] ──raw frame──► SPSC q
                                            │
                                            ▼
                          [InferenceThread] ──landmarks──► SPSC q
                                                              │
                                                              ▼
                                          [StateMachineThread] ──actions──► MPSC q
                                                                                  │
                                                                                  ▼
                                                            [InjectorThread] ──► Win32
                                                                                  │
                                                                                  ▼
                                                                  [RenderThread] ──► Overlay
```

### 1.3 目录结构

```
VMosue/
├── CMakeLists.txt              # 根构建脚本
├── vcpkg.json                  # 依赖清单
├── vcpkg-configuration.json    # 锁定版本
├── README.md
├── LICENSE                     # Apache-2.0
├── .github/workflows/          # CI: windows-2022 + vcpkg
├── docs/
│   ├── superpowers/specs/      # 设计文档（本文档在此）
│   └── user/                   # 用户文档
├── installer/nsi/              # NSIS 打包脚本
├── scripts/                    # bootstrap.ps1, run-dev.ps1
├── resources/
│   ├── icons/                  # 应用图标、托盘图标
│   ├── models/                 # hand_landmarker.task
│   ├── sounds/                 # 点击音效
│   └── config/default.json
├── src/
│   ├── app/                    # main.cpp, App, ThreadPool, Watchdog
│   ├── capture/                # Media Foundation 摄像头采集
│   ├── inference/              # MediaPipe Hands 包装 + 平滑
│   ├── gesture/                # GestureStateMachine + 7 个子检测器
│   ├── input/                  # SendInput 注入
│   ├── ui/                     # Tray, Settings, Overlay, Tutorial, Debug
│   ├── config/                 # JSON 配置 + 校准
│   ├── platform/               # 热键, 开机启动, 多显示器
│   ├── util/                   # Logger, OneEuroFilter, Kalman, Result
│   └── platform_specific/windows/
└── tests/
    ├── unit/                   # GoogleTest
    ├── integration/            # 端到端管线测试
    └── fixtures/
```

---

## 2. 核心模块设计

### 2.1 `capture::CameraCapture`（Media Foundation 封装）

**职责**：枚举摄像头、采集 720p 30/60fps NV12/YUY2 帧、输出统一 `Frame` 结构。

**关键点**：
- 优先使用 `IMFSourceReader` 的低延迟模式
- 帧队列容量 2（最新帧覆盖旧帧）
- 设备拔出/断开时回调 `onDeviceLost` 让 UI 提示
- 通过 `IMFSample` 直接传 NV12 帧给 MediaPipe（避免一次 BGR 转换）

### 2.2 `inference::HandDetector`（MediaPipe Tasks 包装）

**职责**：调用 MediaPipe `HandLandmarker` 检测 0-2 只手、每只 21 个关键点。

**关键点**：
- 默认 GPU 后端（DirectML），失败时自动回退 CPU
- MediaPipe 输出相对坐标，需结合 frame size 还原屏幕坐标
- 推理分辨率降为 640x480（不喂原 720p）以节省 CPU
- 同时输出 `handedness`（0=左手, 1=右手）

### 2.3 `gesture::GestureStateMachine`（核心）

**职责**：消费左右手关键点流，运行 7 个子状态机。

**子状态机**：

| 子状态机 | 输入 | 输出 | 关键阈值 |
|---|---|---|---|
| `CursorController` | 右手 index finger MCP | 鼠标移动 | 5 段灵敏度曲线 + dead zone |
| `ClickDetector` | 拇指+食指距离 | LEFT_CLICK / DBLCLICK | 阈值 ~30px 摄像头，hold 250ms=拖拽 |
| `AirClickDetector` | Z 轴速度 + Y 速度 + 指尖距离 | RIGHT_CLICK | 状态机 IDLE→APPROACH→PEAK→RETREAT，窗口 120-250ms |
| `DragDetector` | 捏合 hold | LEFT_DOWN/UP | 持续 200ms 进入 drag |
| `ScrollDetector` | 左手 2 根手指 Y 位移 | WHEEL | 缩放因子 0.5 |
| `PauseDetector` | 左手张开 hold 1s | PAUSE/RESUME | 1s 阈值 |
| `EmergencyDetector` | 双手张开 OR Ctrl+Alt+G | EMERGENCY_STOP | 500ms |

**关键设计**：每个子状态机独立运行，但共享"全局状态"（`Active`/`Paused`/`Calibrating`/`EmergencyStopped`）。任何子状态机检测到 emergency 都立即将全局状态切到 `EmergencyStopped` 并触发 `safeRelease()`。

### 2.4 `input::InputInjector`（SendInput 封装）

**关键点**：
- 始终用 `SendInput` 而非 `mouse_event`（后者被 Windows 弃用）
- 内部使用相对移动（DX/DY），避免多显示器坐标问题
- 维护 `pressedKeys_` 位图，`SafeReleaseAll` 一次释放
- 1ms 后台线程每 50ms 断言"鼠标按键已释放"，如果没释放就强制 release

### 2.5 `ui::OverlayWindow`（Direct2D 反馈层）

**视觉反馈**：
- 虚拟光标环（默认蓝色圆环）
- 点击波纹（左键=绿色、右键=蓝色、scroll=黄色）
- 状态角标（MOVE/CLICK/DRAG/SCROLL/PAUSE/LOST）
- 置信度颜色：绿（>0.8）/ 黄（0.5-0.8）/ 红（<0.5 或丢失）
- 暂停时显示半透明灰色"手势已暂停"提示

**实现**：
- Win32 Layered Window（`WS_EX_LAYERED` + `WS_EX_TRANSPARENT` 让鼠标穿透）
- Direct2D 渲染 60fps
- 仅在需要反馈时刷新

### 2.6 `ui::TrayIcon` + `ui::SettingsWindow`

**托盘菜单**：
- 启用/暂停手势
- 打开设置
- 打开调试窗口
- 打开手势教程
- 开机启动 ✓
- 退出

**设置面板**（Win32 + 原生控件）：
- 摄像头选择
- 主控手选择
- 灵敏度滑块（移动、滚动）
- 空中点击灵敏度
- 校准按钮
- 性能模式（省电/平衡/性能）
- 关于

### 2.7 `config::Config` + `config::Calibration`

- **Config**：nlohmann/json 序列化，存放在 `%APPDATA%\VMosue\config.json`
- **Calibration**：30-60 秒引导流程，保存个人参数到 `%APPDATA%\VMosue\profiles\<user>.json`

---

## 3. 数据流、错误处理、测试

### 3.1 关键不变量

- 队列永远只保留最新 N 帧（永远处理最新数据，不堆积延迟）
- 每个线程入口有 `try { ... } catch (...) { SafeRelease + Log + 跳过这一帧 }`
- 关键点检测失败（handLost）→ 状态机进入 `LOST` 状态 → Injector 收到 `SafeRelease` 命令 → 释放所有按键
- 摄像头断开 → 状态机进入 `CAMERA_LOST` → Overlay 提示"摄像头不可用" → 5s 后尝试重连

### 3.2 错误处理矩阵

| 错误 | 检测 | 响应 |
|---|---|---|
| 摄像头断开 | Media Foundation 错误回调 | 显示提示 + 尝试重连 + 释放鼠标按键 |
| MediaPipe 模型加载失败 | Init 期 | 设置窗口显示"模型未找到" + 引导下载 |
| MediaPipe 推理崩溃 | try/catch in T2 | 跳过此帧 + spdlog 错误 + 累计 5 次降级到 CPU |
| 状态机状态卡死 | Watchdog 5s 无进展 | 强制 reset 状态机 + 释放鼠标 |
| SendInput 失败 | 返回值检查 | 重试一次 + 失败则 spdlog warn |
| UI 卡死 | Watchdog UI 消息 1s | 提示用户"设置窗口无响应，是否重启" |
| 紧急停止 | 双手张开 OR Ctrl+Alt+G OR Esc 长按 | 立即全局 SafeRelease + Overlay 提示 |
| 配置文件损坏 | JSON 解析 | 备份为 `.bak` + 使用默认配置 |
| 权限不足（管理员窗口） | 注入失败 + GetLastError 5 | 提示"需要管理员权限"按钮 |

### 3.3 Watchdog 设计

- 每个线程每 1s 调用 `Heartbeat(id)`
- 主线程 watchdog 检查 5s 内无心跳的线程
- 触发时调用 `safeRelease` + 在 Overlay 显示红色警告"XX 线程已停止"

### 3.4 测试策略

**单元测试**（GoogleTest，~60% 覆盖目标）：

| 模块 | 测试重点 |
|---|---|
| `OneEuroFilter` | 延迟、抖动抑制、不同 min-cutoff |
| `ClickDetector` | 不同距离/速度的捏合识别 |
| `AirClickDetector` | Z 轴前推/回弹识别、误触情况 |
| `GestureStateMachine` | 状态转移、紧急停止、暂停恢复 |
| `InputInjector` | 模拟输入序列、SafeRelease 完整性 |
| `Config` | 序列化往返、损坏文件回退 |
| `Calibration` | 校准参数生成、边界值 |

**集成测试**：
- 端到端管线（用预录视频 → 验证生成的鼠标动作序列）
- 多显示器坐标变换
- 摄像头热插拔

**手动验收**：

| 任务 | 验收 |
|---|---|
| 点击 20 个随机按钮 | ≥ 99% |
| 双击打开 10 个文件夹 | ≥ 95% |
| 拖拽 10 个图标 | ≥ 95% |
| 滚动网页 5 分钟 | 无明显误触 |
| 右键 20 次 | ≥ 97% |
| 30 分钟连续使用 | 无卡死/按键残留 |
| 丢手/遮挡恢复 | 鼠标不卡 |

**CI**：
- GitHub Actions windows-2022 runner
- 编译 + 单元测试 + 集成测试
- Release 时额外跑：NSIS 打包冒烟、示例视频回放测试

### 3.5 性能预算

| 阶段 | 时间预算 | 优化策略 |
|---|---|---|
| 摄像头采集 → 帧就绪 | < 8ms (60fps) | Media Foundation 低延迟模式 |
| MediaPipe 推理 | < 15ms | GPU DirectML，640x480 输入 |
| 状态机 | < 2ms | 轻量级状态转移 |
| 注入 → 系统响应 | < 8ms | 批量 SendInput |
| Overlay 渲染 | < 8ms | Direct2D 硬件加速 |
| **总 P95** | **< 50ms** | 预留 10ms 余量 |

---

## 4. 实施路线图

虽然目标终点是 v1.0，但实施仍需分阶段：每个阶段可演示、可测试、形成可回滚的 Git tag。

### 4.1 阶段 v0.1 — 技术验证（M1：MVP 内核）

**目标**：证明"摄像头→光标→点击"核心技术路径可用。

| # | 任务 | 验收 |
|---|---|---|
| v0.1.1 | CMake + vcpkg 项目骨架，跑通"Hello Window" | CI 编译通过 |
| v0.1.2 | Media Foundation 摄像头采集，OpenCV 显示预览 | 30fps 预览窗口 |
| v0.1.3 | MediaPipe Tasks C++ 集成，识别单手 21 关键点 | 屏幕打印关键点坐标 |
| v0.1.4 | 右手食指 → 光标移动（无平滑） | 光标能跟随移动 |
| v0.1.5 | 拇指+食指捏合 → 鼠标左键单击 | 捏合一次触发单击 |
| v0.1.6 | 食指空中点击 → 鼠标右键单击 | 空中前推触发右键 |
| v0.1.7 | One Euro Filter 平滑 | 快速移动跟手、停下不抖 |
| v0.1.8 | 基础 Direct2D Overlay（光标环 + 点击波纹） | 视觉反馈清晰 |
| v0.1.9 | SendInput 封装 + SafeRelease | 按键不会残留 |

**Demo**：单手 demo，能点击简单按钮即可。

### 4.2 阶段 v0.2 — 鼠标基本替代（M2：完整鼠标语义）

| # | 任务 | 验收 |
|---|---|---|
| v0.2.1 | 双手识别（区分左右手） | Overlay 标 L/R |
| v0.2.2 | 捏合 hold → 拖拽 | 拖动文件成功 |
| v0.2.3 | 双击时序识别（应用层 250-500ms 窗口） | 触发系统双击 |
| v0.2.4 | 左手 2 指上下 → 滚轮 | 网页滚动正常 |
| v0.2.5 | 多显示器支持（EnumDisplayMonitors + VirtualScreen） | 跨屏光标 |
| v0.2.6 | 暂停/恢复（左手张开 hold 1s） | 状态可切换 |
| v0.2.7 | 紧急停止（双手张开 / Ctrl+Alt+G / Esc 长按） | 立即停止 |
| v0.2.8 | 首次校准（30-60s 引导） | 校准文件生成 |
| v0.2.9 | 配置持久化（nlohmann/json） | 重启保留设置 |
| v0.2.10 | Watchdog + 异常自愈 | 异常后能继续 |
| v0.2.11 | 托盘常驻 + 启停菜单 | 托盘可控 |
| v0.2.12 | spdlog 日志 + 简单诊断页 | 日志可读 |

**Demo**：能完成"打开文件夹→双击进入→拖拽文件→右键菜单→滚动浏览"完整任务链。

### 4.3 阶段 v1.0 — 公开发布（M3：产品化）

| # | 任务 | 验收 |
|---|---|---|
| v1.0.1 | 设置面板 UI（Win32 原生控件） | 全部设置可调 |
| v1.0.2 | 摄像头选择 + 性能模式（省电/平衡/性能） | 多摄可选 |
| v1.0.3 | 调试窗口（实时显示关键点、状态、置信度） | 调试可见 |
| v1.0.4 | 手势教程（交互式引导） | 新用户 5 分钟上手 |
| v1.0.5 | 错误提示完善（中英文 + 错误码） | 错误可读 |
| v1.0.6 | 开机启动（Run 注册表） | 开机自启 |
| v1.0.7 | 性能优化（推理分辨率、空闲降频、ROI 跟踪） | CPU < 25% |
| v1.0.8 | 单元测试 + 集成测试覆盖 | CI 全绿 |
| v1.0.9 | NSIS 安装包（带卸载 + 桌面/开始菜单快捷方式） | 安装/卸载干净 |
| v1.0.10 | 用户文档（README + 教程） | 文档完整 |
| v1.0.11 | 手动验收任务全部跑通 | 全部达标 |
| v1.0.12 | GitHub Release v1.0.0 + 校验和 | 可下载 |

### 4.4 跨阶段"始终进行"

- 持续 spdlog 调试
- 持续性能 profile
- 持续代码 review
- 持续更新文档

### 4.5 风险与里程碑

| 风险 | 影响 | 缓解 |
|---|---|---|
| MediaPipe C++ 在 Windows 上 GPU 后端不稳 | v0.1 延期 | v0.1.3 准备 CPU 回退路径 |
| SendInput 在某些游戏中无效 | v0.2 边界 | 文档明确说明"不支持反作弊游戏" |
| 多显示器坐标变换复杂 | v0.2 延期 | v0.2.5 用 VirtualScreen rect 简单实现 |
| 状态机 bug 难定位 | 跨阶段 | Debug 窗口 + 完整日志 + 单元测试 |

---

## 5. 关键决策记录

| 决策 | 选择 | 备选 | 理由 |
|---|---|---|---|
| 实施起点 | MVP v1.0 | v0.1, v0.2 | 用户选择 |
| 手部识别 | MediaPipe Tasks C++ | ONNX Runtime | 官方实现，21 关键点完整 |
| 构建/依赖 | vcpkg | Conan, 手动 | 跨平台一致、CMake 集成 |
| 采集/渲染 | Media Foundation + Direct2D | OpenCV + ImGui | 原生 API、性能高、占用低 |
| 架构 | 单进程模块化 | 双进程隔离 | 延迟低、部署简单、满足 v1.0 需求 |
| 输入注入 | Win32 SendInput | 虚拟 HID 驱动 | 无需驱动签名、兼容性好 |
| 状态机 | 自定义 + One Euro Filter | 第三方状态机库 | 简单可控、零依赖 |
| 许可证 | Apache-2.0 | MIT | 带专利授权，更适合工程项目 |
| 打包 | NSIS | MSIX, WiX | 开源、灵活、文件小 |
| UI 框架 | 原生 Win32 | WinUI 3, Dear ImGui | 简单、零依赖、托盘+对话框够用 |

---

## 6. 不在范围内（明确排除）

- 远程控制
- 全身姿态识别
- 复杂自定义手势训练
- 浏览器版本
- Electron 桌面壳
- 内核虚拟 HID 驱动
- 控制 UAC 安全桌面、反作弊游戏、管理员权限更高的窗口
- macOS / Linux 平台支持
- 游戏手柄模拟
- 复杂快捷键（复制/粘贴等）

---

## 7. 验收总则

v1.0 公开发布必须满足：

- [ ] 所有 v0.1 / v0.2 / v1.0 任务全部完成
- [ ] P95 端到端延迟 < 60ms
- [ ] CPU 占用 < 25%
- [ ] 内存占用 < 300MB
- [ ] 误触发率 ≤ 1%
- [ ] 手动验收任务全部达标
- [ ] CI 全绿（编译 + 单元 + 集成测试）
- [ ] NSIS 安装包可用、卸载干净
- [ ] 用户文档完整
- [ ] GitHub Release v1.0.0 + 校验和
- [ ] Apache-2.0 LICENSE 文件存在
