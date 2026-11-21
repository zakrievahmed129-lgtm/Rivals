param(
    [switch]$RunTests = $true
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Libs = @("-ld3d11", "-ldxgi", "-ld2d1", "-ldwrite", "-ldcomp", "-ldwmapi", "-lws2_32", "-luser32", "-lgdi32", "-lwinmm")

if (Get-Command g++ -ErrorAction SilentlyContinue) {
    Write-Host "[nah2] Compilation avec g++ (C++20)..." -ForegroundColor Cyan

    & g++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -DNOMINMAX `
        -I"$Root\src" `
        "$Root\src\aim_assist.cpp" `
        "$Root\src\control_panel.cpp" `
        "$Root\src\ipc_receiver.cpp" `
        "$Root\src\renderer.cpp" `
        "$Root\src\nah2.cpp" `
        -o "$BuildDir\nah2.exe" @Libs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & g++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -DNOMINMAX `
        -I"$Root\src" `
        "$Root\src\aim_assist.cpp" `
        "$Root\src\control_panel.cpp" `
        "$Root\src\ipc_receiver.cpp" `
        "$Root\src\renderer.cpp" `
        "$Root\tests\test_nah2.cpp" `
        -o "$BuildDir\test_nah2.exe" @Libs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "[nah2] Binaire genere : $BuildDir\nah2.exe" -ForegroundColor Green

    if ($RunTests) {
        Write-Host "[nah2] Execution des tests unitaires et IPC..." -ForegroundColor Cyan
        & "$BuildDir\test_nah2.exe"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
} else {
    Write-Error "g++ introuvable dans le PATH. Ouvrez un terminal MSYS2 UCRT64 ou utilisez CMake + Visual Studio."
}
