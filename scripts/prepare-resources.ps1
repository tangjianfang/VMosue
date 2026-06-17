<#
.SYNOPSIS
Prepare VMosue runtime resources: download the ML model, install the
Python dependencies, optionally sync resources next to a built binary,
and verify everything is in place.

.DESCRIPTION
vmosue.exe spawns `python scripts/hand_detector_server.py` at runtime,
which loads `resources/models/hand_landmarker.task` and imports
mediapipe + numpy. None of those three are tracked in git (the model is
a ~7 MB binary; the Python packages are environment-specific), so a
fresh checkout cannot run the app until they are fetched. This script
does that in one step.

Run it BEFORE building (so CMake's resource-sync step finds the model
and stops warning about it) or any time AFTER, to repair a checkout.

.PARAMETER ModelUrl
Override the hand_landmarker.task download URL.

.PARAMETER BuildDir
If given (e.g. "build"), also mirror scripts/ and resources/ into
<BuildDir>/bin so a binary built there can find them without CMake's
sync step. Normally unnecessary — CMake already does this on build.

.PARAMETER SkipModel
Skip the model download.

.PARAMETER SkipPython
Skip installing the Python dependencies.

.PARAMETER Python
Python executable to use for pip (default: "python").

.EXAMPLE
.\scripts\prepare-resources.ps1
.EXAMPLE
.\scripts\prepare-resources.ps1 -BuildDir build -Python py
#>
[CmdletBinding()]
param(
    [string]$ModelUrl = "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task",
    [string]$BuildDir = "",
    [switch]$SkipModel,
    [switch]$SkipPython,
    [string]$Python = "python"
)

$ErrorActionPreference = 'Stop'

# Resolve the project root from this script's location (scripts/ is one
# level below root) so the script works regardless of the caller's CWD.
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ModelDir    = Join-Path $ProjectRoot "resources\models"
$ModelPath   = Join-Path $ModelDir   "hand_landmarker.task"
$ReqPath     = Join-Path $ProjectRoot "requirements.txt"

# A valid float16 hand_landmarker.task is several MB; a truncated or
# error-page download is far smaller. Reject anything under this floor
# so a half-finished file isn't mistaken for a good model.
$MinModelBytes = 1MB

function Write-Step($msg)  { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)    { Write-Host "    [ok] $msg" -ForegroundColor Green }
function Write-Warn2($msg) { Write-Host "    [warn] $msg" -ForegroundColor Yellow }

# Run an external command, discard all of its output, and return its
# exit code. Wrapped so a non-zero exit (or a tool writing to stderr)
# does NOT turn into a terminating NativeCommandError under
# $ErrorActionPreference='Stop' — we only want the exit code.
function Invoke-Quiet {
    param([string]$Exe, [string[]]$ExeArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @ExeArgs 2>&1 | Out-Null
        return $LASTEXITCODE
    } catch {
        return 1
    } finally {
        $ErrorActionPreference = $old
    }
}

# ---------------------------------------------------------------------
# 1. ML model
# ---------------------------------------------------------------------
if (-not $SkipModel) {
    Write-Step "Hand-landmarker model"
    New-Item -ItemType Directory -Force -Path $ModelDir | Out-Null

    $haveGoodModel = (Test-Path $ModelPath) -and `
        ((Get-Item $ModelPath).Length -ge $MinModelBytes)

    if ($haveGoodModel) {
        $mb = [math]::Round((Get-Item $ModelPath).Length / 1MB, 1)
        Write-Ok "already present ($mb MB) -> $ModelPath"
    }
    else {
        if (Test-Path $ModelPath) {
            Write-Warn2 "existing file looks truncated; re-downloading"
            Remove-Item $ModelPath -Force
        }
        Write-Host "    downloading from $ModelUrl"
        $tmp = "$ModelPath.download"
        try {
            # Invoke-WebRequest is built in; -UseBasicParsing keeps it
            # working on machines without the IE engine.
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $ModelUrl -OutFile $tmp -UseBasicParsing
        }
        catch {
            if (Test-Path $tmp) { Remove-Item $tmp -Force }
            throw "Model download failed: $($_.Exception.Message)"
        }
        $size = (Get-Item $tmp).Length
        if ($size -lt $MinModelBytes) {
            Remove-Item $tmp -Force
            throw "Downloaded model is only $size bytes (< $MinModelBytes); the URL may be wrong or blocked."
        }
        # Atomic-ish swap into place.
        Move-Item -Force $tmp $ModelPath
        $mb = [math]::Round($size / 1MB, 1)
        Write-Ok "downloaded ($mb MB) -> $ModelPath"
    }
}
else {
    Write-Step "Hand-landmarker model (skipped)"
}

# ---------------------------------------------------------------------
# 2. Python dependencies
# ---------------------------------------------------------------------
# Keep a requirements.txt as the single source of truth so CI and devs
# install the same set. mediapipe pulls numpy transitively, but we pin
# numpy explicitly because the server imports it directly.
$reqContent = @(
    "# Runtime dependencies for scripts/hand_detector_server.py.",
    "# Installed by scripts/prepare-resources.ps1.",
    "mediapipe>=0.10",
    "numpy>=1.24"
)
if (-not (Test-Path $ReqPath)) {
    Set-Content -Path $ReqPath -Value $reqContent -Encoding UTF8
}

if (-not $SkipPython) {
    Write-Step "Python dependencies"
    $pyOk = (Invoke-Quiet $Python @('--version')) -eq 0

    if (-not $pyOk) {
        Write-Warn2 "'$Python' not found on PATH; skipping pip install. Install Python 3.9+ and re-run, or use -Python py."
    }
    else {
        Write-Host "    installing from $ReqPath"
        & $Python -m pip install --quiet --upgrade -r $ReqPath
        if ($LASTEXITCODE -ne 0) {
            throw "pip install failed (exit $LASTEXITCODE)."
        }
        Write-Ok "mediapipe + numpy installed"
    }
}
else {
    Write-Step "Python dependencies (skipped)"
}

# ---------------------------------------------------------------------
# 3. Sync into build/bin (optional; CMake normally handles this)
# ---------------------------------------------------------------------
if ($BuildDir -ne "") {
    Write-Step "Sync runtime data into $BuildDir\bin"
    $binDir = Join-Path $ProjectRoot (Join-Path $BuildDir "bin")
    if (-not (Test-Path $binDir)) {
        Write-Warn2 "$binDir does not exist yet (build first); skipping sync"
    }
    else {
        Copy-Item -Recurse -Force (Join-Path $ProjectRoot "scripts")   $binDir
        Copy-Item -Recurse -Force (Join-Path $ProjectRoot "resources") $binDir
        Write-Ok "scripts/ and resources/ mirrored into $binDir"
    }
}

# ---------------------------------------------------------------------
# 4. Verify
# ---------------------------------------------------------------------
Write-Step "Verification"
$problems = @()

# Model present and non-trivial.
if ((Test-Path $ModelPath) -and (Get-Item $ModelPath).Length -ge $MinModelBytes) {
    Write-Ok "model present"
} else {
    $problems += "model missing or truncated at $ModelPath (run without -SkipModel, or download manually per resources/models/README.txt)"
}

# Server script present.
$serverScript = Join-Path $ScriptDir "hand_detector_server.py"
if (Test-Path $serverScript) {
    Write-Ok "hand_detector_server.py present"
} else {
    $problems += "scripts/hand_detector_server.py missing"
}

# Python interpreter.
$pyResolved = (Invoke-Quiet $Python @('--version')) -eq 0
if ($pyResolved) {
    Write-Ok "python interpreter found ('$Python')"
} else {
    $problems += "python interpreter '$Python' not found on PATH"
}

# mediapipe + numpy importable. Invoke-Quiet swallows the traceback a
# failed import prints to stderr; we only want the exit code.
if ($pyResolved) {
    if ((Invoke-Quiet $Python @('-c', 'import mediapipe, numpy')) -eq 0) {
        Write-Ok "mediapipe + numpy importable"
    } else {
        $problems += "mediapipe/numpy not importable (run without -SkipPython)"
    }
}

Write-Host ""
if ($problems.Count -eq 0) {
    Write-Host "All runtime resources are ready. vmosue.exe can run." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Some resources are NOT ready:" -ForegroundColor Red
    foreach ($p in $problems) { Write-Host "  - $p" -ForegroundColor Red }
    exit 1
}
