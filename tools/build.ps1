param([string]$Toolchain = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $projectRoot
try {
    if ($Toolchain) {
        $compiler = Join-Path $Toolchain 'g++.exe'
        $env:PATH = $Toolchain + ';' + $env:PATH
    } else {
        $compiler = (Get-Command 'g++.exe' -ErrorAction Stop).Source
    }
    if (-not (Test-Path -LiteralPath $compiler)) { throw 'g++.exe was not found; supply -Toolchain with the MSYS2 UCRT64 bin directory' }
    New-Item -ItemType Directory -Force -Path 'build\obj','build\tests' | Out-Null
    $ownSources = @('Source\Overlay.cpp','Source\Runtime.cpp','Source\SubtitleTable.cpp')
    $vendorSources = @('vendor\imgui\imgui.cpp','vendor\imgui\imgui_draw.cpp','vendor\imgui\imgui_tables.cpp','vendor\imgui\imgui_widgets.cpp','vendor\imgui\backends\imgui_impl_dx11.cpp','vendor\imgui\backends\imgui_impl_win32.cpp')
    $vendorSources += Get-ChildItem -LiteralPath 'vendor\Detours\src' -Filter '*.cpp' | Where-Object Name -NE 'uimports.cpp' | ForEach-Object { 'vendor\Detours\src\' + $_.Name }
    $common = @('-std=c++17','-O2','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-D_WIN32_WINNT=0x0601','-ISource','-Ivendor/imgui','-Ivendor/imgui/backends','-Ivendor/Detours/src')
    $objects = @()
    foreach ($source in ($ownSources + $vendorSources)) {
        $object = 'build\obj\' + ($source -replace '[\\/.]','_') + '.o'
        $objects += $object
        Write-Output ('Compiling ' + $source)
        $warnings = if ($source -in $ownSources) { @('-Wall','-Wextra','-Werror') } else { @('-w') }
        $compileArgs = @($common) + @($warnings) + @('-c', $source, '-o', $object)
        & $compiler @compileArgs
        if ($LASTEXITCODE -ne 0) { throw ('Compilation failed: ' + $source) }
    }
    & $compiler '-shared' '-static' '-s' '-o' 'build\subtitles.dll' @objects '-ld3d11' '-ldxgi' '-ld3dcompiler' '-ldwmapi' '-lgdi32' '-limm32' '-lpsapi' '-lversion' '-lbcrypt' '-lole32' '-luuid' '-ladvapi32' '-lws2_32'
    if ($LASTEXITCODE -ne 0) { throw 'DLL link failed' }
    & $compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Werror' '-static' 'tests\subtitle_model_test.cpp' '-o' 'build\tests\subtitle_model_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Playback model test build failed' }
    & $compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Werror' '-static' 'Source\SubtitleTable.cpp' 'tests\subtitle_table_test.cpp' '-o' 'build\tests\subtitle_table_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Subtitle table test build failed' }
    & '.\build\tests\subtitle_model_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Playback model test failed' }
    & '.\build\tests\subtitle_table_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Subtitle table test failed' }
    & $compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-Werror' '-static' 'tests\plugin_load_test.cpp' '-o' 'build\tests\plugin_load_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Plugin load test build failed' }
    & '.\build\tests\plugin_load_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Plugin load/host guard test failed' }
    # The integration fixture includes Runtime.cpp itself and calls its real
    # hooks against fake native functions. It never patches a game process.
    $captureObjects = @($objects | Where-Object { $_ -ne 'build\obj\Source_Runtime_cpp.o' -and $_ -ne 'build\obj\Source_Overlay_cpp.o' })
    & $compiler @common '-Wall' '-Wextra' '-Werror' '-static' 'tests\voice_capture_test.cpp' @captureObjects '-lbcrypt' '-ld3dcompiler' '-ldwmapi' '-lgdi32' '-limm32' '-lpsapi' '-lversion' '-lole32' '-luuid' '-ladvapi32' '-lws2_32' '-o' 'build\tests\voice_capture_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Voice capture fixture build failed' }
    if (Test-Path -LiteralPath 'build\preview\EDF6_VOICETABLE.SUBTITLE.SGO') {
        & '.\build\tests\voice_capture_test.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Voice capture fixture failed' }
    } else { Write-Output 'SKIP optional voice fixture: provide build/preview/EDF6_VOICETABLE.SUBTITLE.SGO' }
    $versionObjects = @($objects | Where-Object { $_ -like '*Detours*' -or $_ -eq 'build\obj\Source_SubtitleTable_cpp.o' })
    & $compiler @common '-Wall' '-Wextra' '-Werror' '-static' 'tests\runtime_version_test.cpp' @versionObjects '-lbcrypt' '-lpsapi' '-o' 'build\tests\runtime_version_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Version fixture build failed' }
    & '.\build\tests\runtime_version_test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Version fixture failed' }
    Get-FileHash -LiteralPath 'build\subtitles.dll' -Algorithm SHA256
} finally { Pop-Location }
