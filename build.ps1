$ErrorActionPreference = "Stop"

$ndk = "C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\28.2.13676358"
$bin = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$imgui = Join-Path $root "imgui"
$src = Join-Path $root "src\mwr_imgui.cpp"
$standaloneSrc = Join-Path $root "src\mwr_standalone.cpp"
$injectSrc = Join-Path $root "src\mwr_inject.cpp"
$extSrc = Join-Path $root "src\mwr_ext_main.cpp"
$extTouchSrc = Join-Path $root "src\external\ExtTouch.cpp"
$outRoot = Join-Path $root "build"

$sources = @(
    $src,
    $standaloneSrc,
    "$imgui\imgui.cpp",
    "$imgui\imgui_draw.cpp",
    "$imgui\imgui_tables.cpp",
    "$imgui\imgui_widgets.cpp",
    "$imgui\backends\imgui_impl_opengl3.cpp"
)

$targets = @(
    @{ Abi = "x86_64";    Compiler = "$bin\x86_64-linux-android29-clang++.cmd" },
    @{ Abi = "arm64-v8a"; Compiler = "$bin\aarch64-linux-android29-clang++.cmd" }
)

foreach ($t in $targets) {
    $abiDir = Join-Path $outRoot $t.Abi
    New-Item -ItemType Directory -Force -Path $abiDir | Out-Null
    $outSo = Join-Path $abiDir "libmwr_imgui.so"

    $args = @(
        "-std=c++17",
        "-O2",
        "-fPIC",
        "-shared",
        "-static-libstdc++",
        "-fvisibility=hidden",
        "-DIMGUI_IMPL_OPENGL_ES3",
        "-I$imgui",
        "-I$imgui\backends"
    ) + $sources + @(
        "-lEGL",
        "-lGLESv3",
        "-landroid",
        "-llog",
        "-ldl",
        "-o", $outSo
    )

    Write-Host "[*] Compiling $outSo ($($t.Abi) Android API 29, Dear ImGui + OpenGL ES 3)..."
    & $t.Compiler @args
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for $($t.Abi) with exit code $LASTEXITCODE"
    }
    $info = Get-Item $outSo
    Write-Host "[+] Built $($t.Abi): $($info.FullName) ($($info.Length) bytes)"
}

# Build arm64-v8a standalone ptrace injector (mwr_inject)
$outInject = Join-Path $outRoot "arm64-v8a\mwr_inject"
$injArgs = @(
    "-std=c++17",
    "-O2",
    "-fPIE",
    "-pie",
    "-static-libstdc++",
    $injectSrc,
    "-o", $outInject
)
Write-Host "[*] Compiling $outInject (arm64-v8a standalone ptrace injector)..."
& "$bin\aarch64-linux-android29-clang++.cmd" @injArgs
if ($LASTEXITCODE -ne 0) {
    throw "Build failed for mwr_inject with exit code $LASTEXITCODE"
}
$injInfo = Get-Item $outInject
Write-Host "[+] Built arm64-v8a injector: $($injInfo.FullName) ($($injInfo.Length) bytes)"

# Build arm64-v8a external Read-Only ESP overlay (mwr_ext)
$outExt = Join-Path $outRoot "arm64-v8a\mwr_ext"
$extArgs = @(
    "-std=c++17",
    "-O2",
    "-fPIE",
    "-pie",
    "-static-libstdc++",
    "-DIMGUI_IMPL_OPENGL_ES3",
    "-I$imgui",
    "-I$imgui\backends",
    "-I$(Join-Path $root 'src')",
    $extSrc,
    $extTouchSrc,
    "$imgui\imgui.cpp",
    "$imgui\imgui_draw.cpp",
    "$imgui\imgui_tables.cpp",
    "$imgui\imgui_widgets.cpp",
    "$imgui\backends\imgui_impl_opengl3.cpp",
    "-lEGL",
    "-lGLESv3",
    "-landroid",
    "-llog",
    "-ldl",
    "-o", $outExt
)
Write-Host "[*] Compiling $outExt (arm64-v8a external Read-Only ESP overlay)..."
& "$bin\aarch64-linux-android29-clang++.cmd" @extArgs
if ($LASTEXITCODE -ne 0) {
    throw "Build failed for mwr_ext with exit code $LASTEXITCODE"
}
$extInfo = Get-Item $outExt
Write-Host "[+] Built arm64-v8a external binary: $($extInfo.FullName) ($($extInfo.Length) bytes)"

# Strip arm64 binaries to keep standalone scripts compact
& "$bin\llvm-strip.exe" --strip-unneeded (Join-Path $outRoot "arm64-v8a\libmwr_imgui.so") $outInject $outExt
if ($LASTEXITCODE -ne 0) {
    throw "llvm-strip failed with exit code $LASTEXITCODE"
}

# Pack single self-contained root scripts (build/mwr_mod.sh + build/mwr_ext.sh)
python (Join-Path $root "pack_sh.py")
if ($LASTEXITCODE -ne 0) {
    throw "pack_sh.py failed with exit code $LASTEXITCODE"
}
python (Join-Path $root "pack_ext_sh.py")
if ($LASTEXITCODE -ne 0) {
    throw "pack_ext_sh.py failed with exit code $LASTEXITCODE"
}

# Keep legacy build/libmwr_imgui.so (x86_64) for existing scripts
Copy-Item -Force (Join-Path $outRoot "x86_64\libmwr_imgui.so") (Join-Path $outRoot "libmwr_imgui.so")
