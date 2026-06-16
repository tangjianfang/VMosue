#include "inference/HandDetector.h"

#include "util/Adaptive.h"
#include "util/Logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace vmosue {

namespace {

// HandLandmarkerWrapper from v0.1..v0.2 has been retired; the v0.3
// implementation drives MediaPipe's official `hand_landmarker.task`
// model from a Python subprocess (see scripts/hand_detector_server.py).
// The subprocess is launched with stdin/stdout piped; per-frame
// protocol is documented at the top of that file. This file only
// deals with process lifecycle + JSON serialization.
}  // namespace

// Walk PATH looking for a real python.exe. We must NOT just trust
// the bare name "python" — Windows 10/11 installs a Microsoft
// Store stub at
//   %LOCALAPPDATA%\Microsoft\WindowsApps\python.exe
// (typically ~5 KB; sometimes zero bytes) which silently exits
// when launched outside the Store sandbox, breaking our pipe.
// The heuristic: prefer `python.exe` (real interpreter binaries
// are > 50 KB), then fall back to `py.exe` (the Windows launcher
// at C:\Windows\py.exe, which handles version selection).
std::string FindPythonExecutable() {
  char pathBuf[32767];
  DWORD n = GetEnvironmentVariableA("PATH", pathBuf, sizeof(pathBuf));
  if (n == 0 || n >= sizeof(pathBuf)) return "";

  std::string best;  // first >= 50KB candidate
  std::string launcher;  // any py.exe candidate
  std::string s(pathBuf, n);
  size_t start = 0;
  while (start < s.size()) {
    size_t end = s.find(';', start);
    if (end == std::string::npos) end = s.size();
    std::string dir = s.substr(start, end - start);
    start = end + 1;
    if (dir.empty()) continue;

    auto tryFile = [&](const char* name, std::string& out,
                       bool requireRealSize) -> bool {
      std::string full = dir + "\\" + name;
      WIN32_FILE_ATTRIBUTE_DATA fad{};
      if (!GetFileAttributesExA(full.c_str(), GetFileExInfoStandard,
                                &fad)) {
        return false;
      }
      if (requireRealSize) {
        // Real CPython python.exe is ~100 KB+. The Windows Store
        // stub is 0..5 KB and silently exits when launched outside
        // the sandbox. Reject files smaller than 50 KB.
        ULONGLONG size =
            (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) |
            fad.nFileSizeLow;
        if (size < 50ULL * 1024) return false;
      }
      out = full;
      return true;
    };

    if (best.empty()) tryFile("python.exe", best, /*requireRealSize=*/true);
    if (launcher.empty()) tryFile("py.exe", launcher,
                                  /*requireRealSize=*/false);
    if (!best.empty() && !launcher.empty()) break;
  }
  if (!best.empty()) return best;
  return launcher;
}

// RAII for the Win32 HANDLEs we own while the subprocess runs.
// Closing the pipes before the child exits will hang the child on
// its next write; we therefore only close the pipes in the
// destructor AFTER the process has been signalled to terminate and
// waited on, or in error paths where we never sent anything.
//
// Move semantics: std::move() on this struct MUST null out the
// source's handles; otherwise the source's destructor (which runs
// when the local variable goes out of scope at the end of
// HandDetector::Init) will CloseHandle() each one, dropping the
// kernel pipe's reference count. The pipe object itself stays
// alive (we still hold a handle via child_->*), but the close()
// bookkeeping around the parent's stdin_write handle gets
// disturbed — without explicit move semantics, std::move on this
// struct falls back to the implicit copy constructor, leaving the
// source fully populated. The bug we'd otherwise chase is
// nondeterministic and depends on whether the copy-elision /
// return-value-optimization elides the copy.
struct vmosue::ChildHandles {
  HANDLE process = nullptr;        // child process handle
  HANDLE stdin_write = nullptr;    // our end of child's stdin pipe
  HANDLE stdout_read = nullptr;    // our end of child's stdout pipe

  ChildHandles() = default;
  ChildHandles(HANDLE p, HANDLE sw, HANDLE sr)
      : process(p), stdin_write(sw), stdout_read(sr) {}

  ChildHandles(const ChildHandles&) = delete;
  ChildHandles& operator=(const ChildHandles&) = delete;

  ChildHandles(ChildHandles&& other) noexcept
      : process(other.process),
        stdin_write(other.stdin_write),
        stdout_read(other.stdout_read) {
    other.process = nullptr;
    other.stdin_write = nullptr;
    other.stdout_read = nullptr;
  }
  ChildHandles& operator=(ChildHandles&& other) noexcept {
    if (this != &other) {
      if (stdin_write) CloseHandle(stdin_write);
      if (stdout_read) CloseHandle(stdout_read);
      if (process)     CloseHandle(process);
      process     = other.process;
      stdin_write = other.stdin_write;
      stdout_read = other.stdout_read;
      other.process     = nullptr;
      other.stdin_write = nullptr;
      other.stdout_read = nullptr;
    }
    return *this;
  }

  ~ChildHandles() {
    if (stdin_write)  CloseHandle(stdin_write);
    if (stdout_read)  CloseHandle(stdout_read);
    if (process) {
      CloseHandle(process);
    }
  }
};

namespace {

// Locate the vmosue project root at runtime. The C++ binary is
// normally run from the build tree (e.g. `build/bin/vmosue.exe`),
// in which case CWD-relative paths like `scripts/hand_...` and
// `resources/models/...` resolve to the wrong place. The original
// v0.3 code required the user to launch from the project root
// (a footgun: a `cd build && ./bin/vmosue.exe` invocation
// immediately failed with "No such file"). This resolver tries
// three strategies, in order:
//
//   1. CWD — works when the user runs from the project root, or
//      when the build tree is laid out so that CWD has both
//      `scripts/hand_detector_server.py` and
//      `resources/models/hand_landmarker.task`.
//   2. The directory containing vmosue.exe (one level up of the
//      binary's path).
//   3. Walk up from the binary's directory looking for the
//      sentinel `scripts/hand_detector_server.py`. The
//      `build/bin/vmosue.exe` -> `build/` -> project root layout
//      is the common case this catches.
//
// Returns an absolute path with no trailing slash, or "" if no
// candidate has the expected layout. The check is "scripts AND
// resources both present" so we don't accidentally claim an
// intermediate directory that has one but not the other.
std::string ResolveProjectRoot() {
  auto hasLayout = [](const std::string& d) -> bool {
    auto fileExists = [](const std::string& p) -> bool {
      DWORD attr = GetFileAttributesA(p.c_str());
      return attr != INVALID_FILE_ATTRIBUTES &&
             !(attr & FILE_ATTRIBUTE_DIRECTORY);
    };
    return fileExists(d + "\\scripts\\hand_detector_server.py") &&
           fileExists(d + "\\resources\\models\\hand_landmarker.task");
  };

  // (1) CWD.
  char cwd[MAX_PATH] = {};
  if (GetCurrentDirectoryA(sizeof(cwd), cwd) > 0) {
    std::string d(cwd);
    if (hasLayout(d)) return d;
  }

  // (2) The directory containing the running executable.
  char exePath[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, exePath, sizeof(exePath)) > 0) {
    std::string d(exePath);
    auto slash = d.find_last_of("\\/");
    if (slash != std::string::npos) d.resize(slash);
    if (hasLayout(d)) return d;

    // (3) Walk up parent directories looking for the sentinel
    // pair. We bound the walk at 8 levels to avoid an infinite
    // loop if the binary is somehow outside a normal tree.
    for (int i = 0; i < 8; ++i) {
      slash = d.find_last_of("\\/");
      if (slash == std::string::npos) break;
      d.resize(slash);
      if (hasLayout(d)) return d;
    }
  }
  return "";
}

// Read a single LF-terminated line from the child's stdout. Defined
// further down in this TU (also used by SpawnPythonDetector's
// post-handshake write probe and the per-frame Detect() call).
bool ReadLine(HANDLE pipe, std::string& line, DWORD timeoutMs);

// Spawn `python <script> [--model <path>] [--num-hands N]`. Returns
// the four HANDLEs needed for IPC. On any failure, the function logs
// the error and returns false with all handles null.
bool SpawnPythonDetector(const std::string& modelPath,
                         int numHands,
                         float minHandConfidence,
                         ChildHandles& out) {
  // Locate a real python.exe. We must NOT pass the bare name
  // "python" because PATH may resolve it to the Windows Store
  // stub (Microsoft\WindowsApps\python.exe), which exits silently.
  // FindPythonExecutable walks PATH and rejects sub-50KB files.
  std::string pythonExe = FindPythonExecutable();
  if (pythonExe.empty()) {
    VMOSUE_LOG_ERROR("HandDetector: no python.exe found on PATH "
                     "(looked for a >= 50KB interpreter and py.exe)");
    return false;
  }
  VMOSUE_LOG_INFO("HandDetector: using python at {}", pythonExe);

  // PYTHONUNBUFFERED=1 forces line-buffered stdout; without it,
  // Python's pipe buffer holds output until 4 KB accumulates or
  // the process exits, which makes our "ready" handshake stall.
  SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");

  // v0.5 (bug fix): resolve the script + model paths against the
  // project root, not CWD. Running `vmosue.exe` from the build
  // tree (e.g. `cd build && ./bin/vmosue.exe`) put CWD at
  // `build/`, so the bare `scripts\hand_detector_server.py`
  // path resolved to `build\scripts\...` (does not exist) and
  // the python child exited with "No such file" before its
  // ready handshake. ResolveProjectRoot() finds the real root
  // by walking up from the executable directory until it
  // finds the `scripts/hand_detector_server.py` + `resources/
  // models/hand_landmarker.task` pair. If we can't find a
  // root, we fall back to the original CWD-relative behavior
  // so the user still gets a clear "file not found" instead
  // of a silent default.
  std::string root = ResolveProjectRoot();
  if (root.empty()) {
    VMOSUE_LOG_WARN("HandDetector: could not locate project root "
                    "(scripts/hand_detector_server.py + resources/"
                    "models/hand_landmarker.task not found relative to "
                    "CWD or vmosue.exe); falling back to CWD-relative");
  } else {
    VMOSUE_LOG_INFO("HandDetector: project root resolved to {}", root);
  }
  std::string scriptPath = root.empty()
      ? std::string("scripts\\hand_detector_server.py")
      : (root + "\\scripts\\hand_detector_server.py");
  std::string modelFull = modelPath;
  if (!root.empty() && (modelPath.find(':') == std::string::npos &&
                        modelPath.find("\\\\") == std::string::npos)) {
    modelFull = root + "\\" + modelPath;
  }

  std::string cmdLine = "\"" + pythonExe +
                        "\" -u \"" + scriptPath + "\" "
                        "--model \"" + modelFull + "\" "
                        "--num-hands " + std::to_string(numHands) +
                        " --min-hand-confidence " +
                        std::to_string(minHandConfidence);

  // Create stdin pipe (we write, child reads) and stdout pipe
  // (child writes, we read). Both pipes need the
  // HANDLE_FLAG_INHERIT flag so the spawned python process inherits
  // the handles.
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  if (!CreatePipe(&child_stdin_read, &out.stdin_write, &sa, 0)) {
    VMOSUE_LOG_ERROR("HandDetector: CreatePipe(stdin) failed: {}",
                     GetLastError());
    return false;
  }
  if (!CreatePipe(&out.stdout_read, &child_stdout_write, &sa, 0)) {
    VMOSUE_LOG_ERROR("HandDetector: CreatePipe(stdout) failed: {}",
                     GetLastError());
    CloseHandle(child_stdin_read);
    CloseHandle(out.stdin_write);
    out.stdin_write = nullptr;
    return false;
  }

  // Ensure the parent's ends of the pipes are NOT inherited by the
  // child (otherwise the child keeps them open and we can't detect
  // EOF cleanly). The child-side handles (child_stdin_read and
  // child_stdout_write) MUST remain inheritable — they are what
  // the child actually reads from / writes to.
  //
  // Bug fix: in an earlier draft we cleared HANDLE_FLAG_INHERIT on
  // the child-side handles by mistake. CreateProcess would then
  // give the child a closed/invalid stdin/stdout, Python would
  // write to a broken pipe, and our parent side would see
  // ERROR_BROKEN_PIPE (109) on the very first PeekNamedPipe call
  // even though the python process was still alive.
  if (!SetHandleInformation(out.stdin_write, HANDLE_FLAG_INHERIT, 0)) {
    VMOSUE_LOG_ERROR("HandDetector: SetHandleInformation(stdin write) failed");
    CloseHandle(child_stdin_read);
    CloseHandle(out.stdin_write); out.stdin_write = nullptr;
    CloseHandle(out.stdout_read); out.stdout_read = nullptr;
    CloseHandle(child_stdout_write);
    return false;
  }
  if (!SetHandleInformation(out.stdout_read, HANDLE_FLAG_INHERIT, 0)) {
    VMOSUE_LOG_ERROR("HandDetector: SetHandleInformation(stdout read) failed");
    CloseHandle(child_stdin_read);
    CloseHandle(out.stdin_write); out.stdin_write = nullptr;
    CloseHandle(out.stdout_read); out.stdout_read = nullptr;
    CloseHandle(child_stdout_write);
    return false;
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdInput  = child_stdin_read;
  si.hStdOutput = child_stdout_write;
  si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);  // inherit parent's stderr
  PROCESS_INFORMATION pi{};

  // CreateProcessA wants a writable command line. The path is
  // resolved by PATH for `python`; the CWD is inherited so the
  // subprocess can find scripts\ and resources\models\ relative to
  // the parent's working directory.
  std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
  cmdBuf.push_back('\0');

  if (!CreateProcessA(
          nullptr, cmdBuf.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
          CREATE_NO_WINDOW, nullptr,
          nullptr,  // inherit CWD
          &si, &pi)) {
    VMOSUE_LOG_ERROR("HandDetector: CreateProcessA failed: {} "
                     "(is python on PATH and scripts\\hand_detector_server.py "
                     "next to vmosue.exe?)",
                     GetLastError());
    CloseHandle(child_stdin_read);
    CloseHandle(out.stdin_write); out.stdin_write = nullptr;
    CloseHandle(out.stdout_read); out.stdout_read = nullptr;
    CloseHandle(child_stdout_write);
    return false;
  }

  // Parent no longer needs its copy of the child ends of the pipes.
  CloseHandle(child_stdin_read);
  CloseHandle(child_stdout_write);

  out.process = pi.hProcess;
  // We don't keep pi.hThread; close it now.
  CloseHandle(pi.hThread);

  VMOSUE_LOG_INFO("HandDetector: child PID={}, waiting for ready...",
                  pi.dwProcessId);

  // Wait for the child's "ready" handshake. We read one line and
  // parse it; if it doesn't arrive in ~15s (model load + XNNPACK
  // delegate init can take a few seconds on a cold start), we kill
  // the child and report failure.
  char buf[4096];
  DWORD got = 0;
  auto deadline = GetTickCount64() + 15000;
  std::string line;
  while (GetTickCount64() < deadline) {
    DWORD avail = 0;
    if (!PeekNamedPipe(out.stdout_read, nullptr, 0, nullptr, &avail,
                       nullptr)) {
      DWORD err = GetLastError();
      DWORD exitCode = 0;
      GetExitCodeProcess(out.process, &exitCode);
      VMOSUE_LOG_ERROR(
          "HandDetector: PeekNamedPipe failed: {} (err); child exitCode={} "
          "(still_active={})",
          err, exitCode, exitCode == STILL_ACTIVE);
      TerminateProcess(out.process, 1);
      return false;
    }
    if (avail > 0) {
      if (!ReadFile(out.stdout_read, buf, sizeof(buf) - 1, &got, nullptr)) {
        VMOSUE_LOG_ERROR("HandDetector: ReadFile failed: {}",
                         GetLastError());
        TerminateProcess(out.process, 1);
        return false;
      }
      buf[got] = '\0';
      line.append(buf, got);
      auto nl = line.find('\n');
      if (nl != std::string::npos) {
        line.resize(nl);
        break;
      }
    } else {
      // Check if the child has already exited.
      DWORD exitCode = 0;
      if (GetExitCodeProcess(out.process, &exitCode) &&
          exitCode != STILL_ACTIVE) {
        VMOSUE_LOG_ERROR("HandDetector: python child exited with code {} "
                         "before handshake", exitCode);
        return false;
      }
      Sleep(50);
    }
  }
  if (line.empty()) {
    VMOSUE_LOG_ERROR("HandDetector: timed out waiting for python ready line");
    TerminateProcess(out.process, 1);
    return false;
  }
  try {
    auto j = nlohmann::json::parse(line);
    if (j.value("status", "") != "ready") {
      VMOSUE_LOG_ERROR("HandDetector: python handshake not 'ready': {}",
                       line);
      TerminateProcess(out.process, 1);
      return false;
    }
  } catch (const std::exception& exc) {
    VMOSUE_LOG_ERROR("HandDetector: bad python handshake JSON: {} ({})",
                     line, exc.what());
    TerminateProcess(out.process, 1);
    return false;
  }
  VMOSUE_LOG_INFO("HandDetector: python subprocess ready ({})", line);
  return true;
}

// Read a single LF-terminated JSON line from the child's stdout.
// Returns false if EOF or any I/O error. `line` includes the LF; we
// strip it for the caller. The deadline protects against a stuck
// child wedging the App's inference loop.
bool ReadLine(HANDLE pipe, std::string& line, DWORD timeoutMs) {
  char buf[8192];
  DWORD got = 0;
  auto deadline = GetTickCount64() + timeoutMs;
  line.clear();
  while (GetTickCount64() < deadline) {
    DWORD avail = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
      return false;
    }
    if (avail > 0) {
      DWORD want = (DWORD)std::min<size_t>(avail, sizeof(buf) - 1);
      if (!ReadFile(pipe, buf, want, &got, nullptr)) return false;
      buf[got] = '\0';
      line.append(buf, got);
      auto nl = line.find('\n');
      if (nl != std::string::npos) {
        line.resize(nl);
        return true;
      }
      // Guard against a child that emits no LF ever.
      if (line.size() > 1 * 1024 * 1024) return false;
    } else {
      Sleep(5);
    }
  }
  return false;
}

}  // namespace

Result<void> HandDetector::Init(const Config& cfg) {
  cfg_ = cfg;
  if (cfg_.modelPath.empty()) {
    return Result<void>::Err("HandDetector::Init: modelPath is empty");
  }
  ChildHandles h;
  if (!SpawnPythonDetector(cfg_.modelPath, cfg_.maxHands,
                            cfg_.minHandConfidence, h)) {
    return Result<void>::Err(
        "HandDetector::Init: failed to spawn python hand_detector_server.py");
  }
  // Transfer ownership to the member field. We heap-allocate so
  // the destructor (in this TU, with the complete type visible)
  // can run cleanly. The unique_ptr approach wouldn't compile
  // because the dtor of std::unique_ptr<ChildHandles> needs the
  // complete type at the point where HandDetector's dtor is
  // instantiated (including in test TUs that only see the header).
  child_ = new ChildHandles(std::move(h));
  initialized_ = true;
  VMOSUE_LOG_INFO(
      "HandDetector: real MediaPipe Hands via Python subprocess "
      "(model={}, numHands={}, gpu={}, infer={}x{})",
      cfg_.modelPath, cfg_.maxHands,
      cfg_.useGpu ? "on" : "off",
      cfg_.inferenceWidth, cfg_.inferenceHeight);
  return Result<void>::Ok({});
}

std::vector<HandLandmarks> HandDetector::Detect(const Frame& frame) {
  std::vector<HandLandmarks> out;
  if (!initialized_ || !child_) return out;
  if (frame.empty() || frame.format != PixelFormat::BGR24 &&
      frame.format != PixelFormat::RGBA32) {
    return out;
  }

  // CameraCapture emits BGRA frames; this is the path we expect. We
  // also accept BGR24 as a defensive fallback (some test fixtures
  // use it). The Python server expects BGRA so we re-pack BGR24 if
  // needed.
  const uint8_t* bgra = frame.data.data();
  size_t bgra_bytes = static_cast<size_t>(frame.width) *
                      static_cast<size_t>(frame.height) * 4u;
  std::vector<uint8_t> bgra_owned;
  if (frame.format == PixelFormat::BGR24) {
    bgra_owned.resize(static_cast<size_t>(frame.width) *
                      static_cast<size_t>(frame.height) * 4u);
    const uint8_t* src = frame.data.data();
    for (size_t i = 0, j = 0; i < bgra_owned.size();
         i += 4, j += 3) {
      bgra_owned[i + 0] = src[j + 0];  // B
      bgra_owned[i + 1] = src[j + 1];  // G
      bgra_owned[i + 2] = src[j + 2];  // R
      bgra_owned[i + 3] = 255;         // A
    }
    bgra = bgra_owned.data();
    bgra_bytes = bgra_owned.size();
  }

  // ---- Send metadata line ----
  nlohmann::json meta;
  meta["width"]  = static_cast<int>(frame.width);
  meta["height"] = static_cast<int>(frame.height);
  meta["length"] = static_cast<int>(bgra_bytes);
  std::string meta_line = meta.dump() + "\n";
  DWORD wrote = 0;
  if (!WriteFile(child_->stdin_write, meta_line.data(),
                 static_cast<DWORD>(meta_line.size()), &wrote, nullptr) ||
      wrote != meta_line.size()) {
    VMOSUE_LOG_ERROR("HandDetector: WriteFile(meta) failed: {}",
                     GetLastError());
    return out;
  }
  // ---- Send BGRA bytes ----
  // WriteFile on a pipe can short-write if the OS splits the buffer;
  // loop until the whole payload is out.
  size_t sent = 0;
  while (sent < bgra_bytes) {
    DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(bgra_bytes - sent, 1u << 20));  // 1MB chunks
    if (!WriteFile(child_->stdin_write, bgra + sent, chunk, &wrote, nullptr)) {
      VMOSUE_LOG_ERROR("HandDetector: WriteFile(frame) failed at "
                       "{}/{} bytes: {}",
                       sent, bgra_bytes, GetLastError());
      return out;
    }
    if (wrote == 0) {
      VMOSUE_LOG_ERROR("HandDetector: WriteFile stalled at {}/{} bytes",
                       sent, bgra_bytes);
      return out;
    }
    sent += wrote;
  }

  // ---- Read response line (5s budget per frame; 30Hz cap means
  // a healthy frame round-trips in ~80ms) ----
  std::string line;
  if (!ReadLine(child_->stdout_read, line, 5000)) {
    VMOSUE_LOG_ERROR("HandDetector: ReadFile(response) failed/timeout");
    return out;
  }

  // ---- Parse JSON response ----
  try {
    auto j = nlohmann::json::parse(line);
    int hand_count = j.value("hand_count", 0);

    // v0.5: capture the top-2 scores BEFORE filtering so the
    // adaptive controller's score-gap statistic stays accurate even
    // when both detections are below floor (a phantom-heavy scene
    // should still inform the next threshold, not be hidden).
    float top1 = 0.0f, top2 = 0.0f;
    {
      const auto& hs = j.value("hands", nlohmann::json::array());
      for (const auto& h : hs) {
        float s = h.value("score", 0.0f);
        if (s > top1) { top2 = top1; top1 = s; }
        else if (s > top2) { top2 = s; }
      }
    }
    GetSignalObserver().RecordScores(top1, top2);

    // v0.5: phantom filter is now adaptive. The score-gap drives
    // the floor; when the gap is large (one real hand + one phantom)
    // the threshold drops to reject the phantom, and when both hands
    // are genuinely detected (small gap) the threshold stays low so
    // both pass. cfg_.minHandConfidence is no longer consulted here;
    // it's kept only as an absolute hard floor (0.3) inside the
    // adaptive controller.
    float adaptiveFloor = GetAdaptive().MinHandScore();
    out.reserve(hand_count);
    for (const auto& h : j.value("hands", nlohmann::json::array())) {
      HandLandmarks hl;
      std::string hand = h.value("handedness", "Right");
      hl.handedness = (hand == "Left") ? 0 : 1;
      hl.score = h.value("score", 0.0f);
      if (hl.score < adaptiveFloor) continue;
      const auto& lms = h.value("landmarks", nlohmann::json::array());
      int n = std::min<int>(static_cast<int>(lms.size()),
                            HandLandmarks::kNumPoints);
      for (int i = 0; i < n; ++i) {
        const auto& p = lms[i];
        // The Python server emits each landmark as a 3-element
        // array `[x, y, z]` (smaller payload, ~3x faster to parse
        // than the object form). An earlier version of the
        // parser read `p.value("x", 0.0)` which threw a
        // `type_error.306: cannot use value() with array` on
        // every detected hand — the symptom was "no gestures
        // detected" because the C++ side silently returned an
        // empty vector on the parse exception. The element
        // indices below are the canonical 3D coordinates per
        // MediaPipe Hands; `z` is depth (negative = closer to
        // camera) and is left as 0 in `world` meters by the
        // HandLandmarker lite model. Both array and object
        // formats are accepted so a future server tweak doesn't
        // regress this code path.
        if (p.is_array() && p.size() >= 3) {
          hl.points[i].x = p[0].get<double>();
          hl.points[i].y = p[1].get<double>();
          hl.points[i].z = p[2].get<double>();
        } else if (p.is_object()) {
          hl.points[i].x = p.value("x", 0.0);
          hl.points[i].y = p.value("y", 0.0);
          hl.points[i].z = p.value("z", 0.0);
        }
      }
      out.push_back(std::move(hl));
    }
  } catch (const std::exception& exc) {
    VMOSUE_LOG_ERROR("HandDetector: bad response JSON: {} ({})",
                     line.substr(0, 200), exc.what());
    return out;
  }
  return out;
}

HandDetector::~HandDetector() {
  if (child_) {
    if (child_->process) {
      // Send EOF on stdin so the python server's main loop returns
      // 0; if the child is stuck, force-terminate after a short
      // grace.
      if (child_->stdin_write) {
        CloseHandle(child_->stdin_write);
        child_->stdin_write = nullptr;
      }
      DWORD waitResult = WaitForSingleObject(child_->process, 2000);
      if (waitResult == WAIT_TIMEOUT) {
        VMOSUE_LOG_WARN("HandDetector: python child did not exit on EOF; "
                        "TerminateProcess");
        TerminateProcess(child_->process, 1);
        WaitForSingleObject(child_->process, 2000);
      }
    }
    delete child_;
    child_ = nullptr;
  }
}

}  // namespace vmosue