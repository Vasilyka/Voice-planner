$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$env:PYTHONIOENCODING = 'utf-8'
py -m uvicorn main:app --host 0.0.0.0 --port 8000 --no-access-log
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
