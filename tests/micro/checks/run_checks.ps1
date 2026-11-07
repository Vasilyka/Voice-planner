$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Force .build | Out-Null
    g++ -std=c++17 -Wall -Wextra -pedantic checks/audio_format_test.cpp -o .build/audio_format_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Ошибка сборки C++-теста.' }
    & ./.build/audio_format_test.exe .build/fixture.wav
    if ($LASTEXITCODE -ne 0) { throw 'Ошибка проверки PCM/WAV.' }
    py checks/server_test.py
    if ($LASTEXITCODE -ne 0) { throw 'Ошибка HTTP-тестов.' }
} finally {
    Pop-Location
}
