$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    New-Item -ItemType Directory -Force .build | Out-Null
    foreach ($name in @('status_display_test', 'planner_protocol_test')) {
        g++ -std=c++17 -Wall -Wextra -pedantic -Istubs "$name.cpp" -o ".build/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Ошибка сборки $name" }
        & "./.build/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Ошибка проверки $name" }
    }
} finally {
    Pop-Location
}
