# Windows entry point: build the firmware (wraps scripts/build.sh).
#   .\scripts\build.ps1
& (Join-Path $PSScriptRoot 'run-sh.ps1') 'build.sh' @args
exit $LASTEXITCODE
