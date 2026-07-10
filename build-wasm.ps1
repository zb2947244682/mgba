<#
.SYNOPSIS
  build-wasm.ps1 - 把 mGBA 的 GBA 核心 + wasm/mgba-wasm.c 桥接编译成 mgba.js / mgba.wasm

.DESCRIPTION
  Windows + PowerShell 一键编译脚本（替代旧的 build-wasm.sh）。
  会自动定位 emsdk / cmake / ninja，绕开 Windows 微软商店的 python 桩，
  用 CMake + Ninja 交叉编译出 libmgba.a，再用 emcc 链接成 wasm。

  设计要点（详见 WASM_BUILD_SUMMARY.md / WASM_REBUILD_PROMPT.md）：
  - LIBMGBA_ONLY + 仅 GBA + 关 pthread/debugger/scripting。
  - CFLAGS 必须带 -DPATH_MAX=4096 -D_GNU_SOURCE，否则 Emscripten 下 PATH_MAX/strdup 缺失。
  - 必须 -DDISABLE_THREADING -DDISABLE_ANON_MMAP，兼容 Emscripten 内存模型。
  - 桥接器 emcc 命令的宏定义必须与 libmgba.a 一致（本脚本自动从 CMake flags.make 读取，不硬编码）。
  - EXPORTED_RUNTIME_METHODS 含 addFunction/removeFunction，为后续 SIO 联机驱动的 JS->C 回调预留。

.PARAMETER EmSdkDir
  emsdk 安装目录，默认 D:\Codes\emsdk

.PARAMETER DebugBuild
  切换为调试构建（-O0 -g3, ASSERTIONS=2），默认发布构建（-O3）

.EXAMPLE
  .\build-wasm.ps1
  .\build-wasm.ps1 -DebugBuild
#>
[CmdletBinding()]
param(
    [string]$EmSdkDir = "D:\Codes\emsdk",
    [switch]$DebugBuild
)

$ErrorActionPreference = "Stop"

# ---------- 路径 ----------
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$MgbaDir     = if ($PSBoundParameters.ContainsKey('MgbaDir')) { $MgbaDir } else { $ScriptDir }
$BuildDir    = Join-Path $MgbaDir "build-wasm"
$OutDir      = Join-Path $MgbaDir "wasm"
$PyScripts   = Join-Path $env:LOCALAPPDATA "Python\pythoncore-3.14-64\Scripts"

function Write-Step($msg) { Write-Host "[build-wasm] $msg" -ForegroundColor Cyan }
function Write-Err($msg)  { Write-Host "[build-wasm][ERROR] $msg" -ForegroundColor Red }

# ---------- 定位 cmake ----------
function Find-Cmake {
    foreach ($c in @(
        (Join-Path $PyScripts "cmake.exe"),
        (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    )) { if ($c -and (Test-Path $c)) { return $c } }
    return $null
}
# ---------- 定位 ninja ----------
function Find-Ninja {
    foreach ($c in @(
        (Join-Path $PyScripts "ninja.exe"),
        (Get-Command ninja -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    )) { if ($c -and (Test-Path $c)) { return $c } }
    return $null
}

$CmakeBin = Find-Cmake
$NinjaBin = Find-Ninja
if (-not $CmakeBin) { Write-Err "未找到 cmake。请执行：pip install cmake，或把 cmake 加入 PATH。"; exit 1 }
if (-not $NinjaBin) { Write-Err "未找到 ninja。请执行：pip install ninja，或把 ninja 加入 PATH。"; exit 1 }

# ---------- 校验 emsdk ----------
if (-not (Test-Path (Join-Path $EmSdkDir "emsdk_env.ps1"))) {
    Write-Err "未找到 $EmSdkDir\emsdk_env.ps1。请先安装 emsdk 到 $EmSdkDir。"
    exit 1
}
$ToolchainFile = Join-Path $EmSdkDir "upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"
if (-not (Test-Path $ToolchainFile)) {
    Write-Err "未找到 Emscripten 工具链文件：$ToolchainFile"
    Write-Err "说明 emsdk 工具链尚未安装。请在 $EmSdkDir 下执行：.\emsdk install latest ; .\emsdk activate latest"
    exit 1
}

# ---------- 激活 emsdk ----------
# emsdk.ps1 自己会用自带 python 生成 emsdk_set_env.ps1，但它设的 PATH 不含 python 目录。
# 而 emcc 是 python 脚本、emcc.bat 会调 PATH 里的 python -> 会撞上微软商店桩。
# 因此激活后再显式把 emsdk 自带 python 目录塞到 PATH 最前。
Write-Step "激活 emsdk: $EmSdkDir"
$env:EMSDK = $EmSdkDir
& (Join-Path $EmSdkDir "emsdk_env.ps1")
if ($LASTEXITCODE -ne 0) { Write-Err "激活 emsdk 失败。"; exit 1 }

$EmsdkPython = Get-ChildItem (Join-Path $EmSdkDir "python\*\python.exe") -ErrorAction SilentlyContinue | Select-Object -First 1
if ($EmsdkPython) {
    $env:EMSDK_PYTHON = $EmsdkPython.FullName
    $env:PATH = "$($EmsdkPython.DirectoryName);$env:PATH"
} else {
    Write-Err "未找到 emsdk 自带 python（$EmSdkDir\python\*\python.exe）。"
    exit 1
}

# 新版 emsdk 用 emcc.exe（旧版是 emcc.bat），两者都兼容一下。
$Emcc = Join-Path $EmSdkDir "upstream\emscripten\emcc.exe"
if (-not (Test-Path $Emcc)) {
    $Emcc = Join-Path $EmSdkDir "upstream\emscripten\emcc.bat"
}
if (-not (Test-Path $Emcc)) { Write-Err "未找到 emcc：$EmSdkDir\upstream\emscripten\emcc(.exe/.bat)"; exit 1 }

Write-Step "工具版本："
Write-Host ("  cmake : {0}" -f (& $CmakeBin --version | Select-Object -First 1))
Write-Host ("  ninja : {0}" -f (& $NinjaBin --version))
Write-Host ("  emcc  : {0}" -f (& $Emcc --version | Select-Object -First 1))
Write-Host ("  build : {0}" -f $BuildDir)
Write-Host ("  out   : {0}" -f $OutDir)

# ---------- 编译级别 ----------
if ($DebugBuild) {
    $OptFlags  = @("-O0", "-g3")
    $Assertions = 2
    $BuildType  = "Debug"
} else {
    $OptFlags  = @("-O3")
    $Assertions = 0
    $BuildType  = "Release"
}

# ---------- 配置 ----------
Write-Step "配置 CMake（LIBMGBA_ONLY + 仅 GBA 核心）..."
& $CmakeBin -S $MgbaDir -B $BuildDir -G Ninja `
    -DCMAKE_MAKE_PROGRAM="$NinjaBin" `
    -DCMAKE_TOOLCHAIN_FILE="$ToolchainFile" `
    -DCMAKE_BUILD_TYPE="$BuildType" `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
    -DLIBMGBA_ONLY=ON `
    -DM_CORE_GBA=ON `
    -DM_CORE_GB=OFF `
    -DUSE_PTHREADS=OFF `
    -DENABLE_DEBUGGERS=OFF `
    -DENABLE_SCRIPTING=OFF `
    -DBUILD_STATIC=ON `
    -DBUILD_SHARED=OFF `
    -DDISABLE_DEPS=ON `
    -DDISABLE_FRONTENDS=ON `
    "-DCMAKE_C_FLAGS=-DPATH_MAX=4096 -D_GNU_SOURCE -DDISABLE_THREADING -DDISABLE_ANON_MMAP"
if ($LASTEXITCODE -ne 0) { Write-Err "CMake 配置失败。"; exit 1 }

# ---------- 编译静态库 ----------
Write-Step "编译 mGBA 核心静态库（libmgba.a）..."
& $CmakeBin --build $BuildDir --target mgba
if ($LASTEXITCODE -ne 0) { Write-Err "编译 libmgba.a 失败。"; exit 1 }

$LibmgbaA = Get-ChildItem (Join-Path $BuildDir "libmgba.a") -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $LibmgbaA) {
    Write-Err "编译完成但未找到 libmgba.a（$BuildDir）。"
    exit 1
}
Write-Step "找到静态库：$($LibmgbaA.FullName)"

# ---------- 从 CMake flags.make 读取桥接器必须一致的宏定义 ----------
# 桥接器 mgba-wasm.c 的宏必须与 libmgba.a 完全一致，否则 mCore 虚表布局不一致会运行时崩溃。
# 不硬编码，直接读 CMake 生成的 flags.make 里的 C_DEFINES，自动适配本机 feature-check 结果。
$FlagsMake = Join-Path $BuildDir "CMakeFiles\mgba.dir\flags.make"
$BridgeDefines = @()
if (Test-Path $FlagsMake) {
    $line = Get-Content $FlagsMake | Where-Object { $_ -match '^C_DEFINES' } | Select-Object -First 1
    if ($line -match '=\s*(.+)$') {
        $BridgeDefines = ($Matches[1] -split '\s+' | Where-Object { $_ -match '^-D' })
        Write-Step "桥接器宏定义（来自 flags.make，共 $($BridgeDefines.Count) 个）"
    }
}
if ($BridgeDefines.Count -eq 0) {
    Write-Step "未读到 flags.make 的 C_DEFINES，回落到已知宏集合"
    $BridgeDefines = @(
        "-DBUILD_STATIC","-DENABLE_DIRECTORIES","-DENABLE_VFS","-DENABLE_VFS_FD",
        "-DHAVE_FREELOCALE","-DHAVE_FUTIMENS","-DHAVE_LOCALE","-DHAVE_LOCALTIME_R",
        "-DHAVE_NEWLOCALE","-DHAVE_REALPATH","-DHAVE_SETLOCALE","-DHAVE_STRDUP",
        "-DHAVE_STRLCPY","-DHAVE_STRNDUP","-DHAVE_STRTOF_L","-DHAVE_USELOCALE",
        "-DHAVE_VASPRINTF","-DHAVE_XLOCALE","-DM_CORE_GBA"
    )
}

# ---------- 链接 wasm ----------
$null = New-Item -ItemType Directory -Force -Path $OutDir
Write-Step "链接 wasm -> $OutDir\mgba.js (+ mgba.wasm)"

$RuntimeMethods = "['cwrap','ccall','getValue','setValue','UTF8ToString','UTF8ArrayToString','stringToUTF8','lengthBytesUTF8','writeArrayToMemory','addFunction','removeFunction','HEAP8','HEAPU8','HEAP16','HEAPU16','HEAP32','HEAPU32']"
# _malloc / _free 是 C 符号，emscripten 6.0 不接受放进 EXPORTED_RUNTIME_METHODS，
# 改走 EXPORTED_FUNCTIONS（与 EMSCRIPTEN_KEEPALIVE 标注的 mgba_* 导出合并）。
$ExportedFunctions = "['_malloc','_free']"

& $Emcc (Join-Path $MgbaDir "wasm\mgba-wasm.c") $LibmgbaA.FullName `
    "-I$(Join-Path $MgbaDir 'include')" `
    "-I$(Join-Path $MgbaDir 'src')" `
    "-I$(Join-Path $BuildDir 'include')" `
    @($BridgeDefines) `
    @($OptFlags) `
    -s WASM=1 `
    -s MODULARIZE=1 `
    -s EXPORT_NAME=createModule `
    -s EMULATE_FUNCTION_POINTER_CASTS=1 `
    -s ALLOW_MEMORY_GROWTH=1 `
    -s ALLOW_TABLE_GROWTH=1 `
    -s ENVIRONMENT=web,node `
    -s ASSERTIONS=$Assertions `
    "-sEXPORTED_RUNTIME_METHODS=$RuntimeMethods" `
    "-sEXPORTED_FUNCTIONS=$ExportedFunctions" `
    -o (Join-Path $OutDir "mgba.js")
if ($LASTEXITCODE -ne 0) { Write-Err "emcc 链接失败。"; exit 1 }

Write-Step "完成。产物："
Get-ChildItem (Join-Path $OutDir "mgba.js"), (Join-Path $OutDir "mgba.wasm") |
    ForEach-Object { Write-Host ("  {0,-14} {1,10:N0} bytes  {2}" -f $_.Name, $_.Length, $_.FullName) }
Write-Step "下一步自检：node wasm\test-node.js（需自备 ROM 路径）"
