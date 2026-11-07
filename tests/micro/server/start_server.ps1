$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$env:PYTHONIOENCODING = 'utf-8'
py -m uvicorn main:app --host 0.0.0.0 --port 8000
if ($LASTEXITCODE -ne 0) { throw 'Не удалось запустить сервер.' }
