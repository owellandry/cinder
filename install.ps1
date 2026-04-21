# ─────────────────────────────────────────────────────────────────────────────
#  cinder — build & install script
#  Usage:  .\install.ps1
# ─────────────────────────────────────────────────────────────────────────────

$ErrorActionPreference = "Stop"
$Root      = $PSScriptRoot
$BuildDir  = "$Root\build"
$BinDir    = "$env:USERPROFILE\.cinder\bin"
$Exe       = "$BuildDir\cinder.exe"
$Dest      = "$BinDir\cinder.exe"

$sw = [System.Diagnostics.Stopwatch]::StartNew()

function Print-Step($msg) {
    Write-Host "  " -NoNewline
    Write-Host $msg -ForegroundColor Cyan
}
function Print-Ok($msg) {
    Write-Host "  " -NoNewline
    Write-Host "+ " -ForegroundColor Green -NoNewline
    Write-Host $msg
}
function Print-Err($msg) {
    Write-Host "  " -NoNewline
    Write-Host "! " -ForegroundColor Red -NoNewline
    Write-Host $msg
}

Write-Host ""
Write-Host "  cinder build & install" -ForegroundColor White
Write-Host "  ─────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host ""

# ── 1. Verify CMake build dir exists ─────────────────────────────────────────
if (-not (Test-Path "$BuildDir\Makefile")) {
    Print-Step "Configuring CMake..."
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    Push-Location $BuildDir
    try {
        cmake .. -G "MinGW Makefiles" `
            -DCMAKE_BUILD_TYPE=Release `
            -DCMAKE_C_COMPILER="C:/msys64/mingw64/bin/gcc.exe" 2>&1 | Out-Null
        Print-Ok "CMake configured"
    } catch {
        Print-Err "CMake configure failed: $_"
        exit 1
    }
    Pop-Location
}

# ── 2. Kill running cinder instances ─────────────────────────────────────────
$running = Get-Process -Name "cinder" -ErrorAction SilentlyContinue
foreach ($p in $running) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
if ($running.Count -gt 0) {
    Print-Ok "Stopped $($running.Count) running instance(s)"
}

# ── 3. Build ──────────────────────────────────────────────────────────────────
Print-Step "Building..."
Push-Location $BuildDir
$buildOutput = mingw32-make -j4 2>&1
$buildExit   = $LASTEXITCODE
Pop-Location

$errors = $buildOutput | Select-String "error:" | Where-Object { $_ -notmatch "warning" }
if ($buildExit -ne 0 -or $errors.Count -gt 0) {
    Print-Err "Build failed:"
    $errors | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    exit 1
}
Print-Ok "Build succeeded"

# ── 4. Install ───────────────────────────────────────────────────────────────
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
Copy-Item $Exe $Dest -Force
Print-Ok "Installed  $Dest"

# ── 5. Add to PATH if missing ────────────────────────────────────────────────
$userPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($userPath -notlike "*\.cinder\bin*") {
    [Environment]::SetEnvironmentVariable("PATH", "$BinDir;$userPath", "User")
    Print-Ok "Added $BinDir to PATH"
} else {
    Print-Ok "PATH already contains $BinDir"
}

# ── 6. Verify ────────────────────────────────────────────────────────────────
$ver = & $Dest --version 2>&1
$size = [math]::Round((Get-Item $Dest).Length / 1KB)
$sw.Stop()

Write-Host ""
Write-Host "  ─────────────────────────────────────────" -ForegroundColor DarkGray
Print-Ok "$ver  ($size KB)  [$($sw.ElapsedMilliseconds)ms]"
Write-Host ""
Write-Host "  Run: " -NoNewline -ForegroundColor DarkGray
Write-Host "cinder --help" -ForegroundColor White
Write-Host ""
