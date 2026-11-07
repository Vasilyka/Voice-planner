param([string]$Python = 'py')
$ErrorActionPreference = 'Stop'
$pythonCommand = (Get-Command $Python -ErrorAction Stop).Source
Push-Location -LiteralPath $PSScriptRoot
try {
    New-Item -ItemType Directory -Force .build | Out-Null
    g++ -std=c++17 -Wall -Wextra -pedantic tests/audio_format_test.cpp -o .build/audio_format_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Ошибка сборки проверки PCM/WAV.' }
    & ./.build/audio_format_test.exe .build/fixture.wav
    if ($LASTEXITCODE -ne 0) { throw 'Ошибка проверки PCM/WAV.' }
    & $pythonCommand tests/server_test.py
    if ($LASTEXITCODE -ne 0) { throw 'Ошибка HTTP-тестов.' }
} finally {
    Pop-Location
}
