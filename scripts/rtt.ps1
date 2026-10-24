# Windows entry point: grab RTT log from the board (wraps scripts/rtt.sh).
#   .\scripts\rtt.ps1
#   .\scripts\rtt.ps1 --file out.txt
#   .\scripts\rtt.ps1 --speed 2000
# Ctrl-C to stop. Do NOT type scripts\rtt.sh directly in PowerShell -
# that opens the file in VS Code instead of running it.
& (Join-Path $PSScriptRoot 'run-sh.ps1') 'rtt.sh' @args
exit $LASTEXITCODE
