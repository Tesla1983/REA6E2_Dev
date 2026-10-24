# Windows entry point: flash the firmware (wraps scripts/flash.sh).
#   .\scripts\flash.ps1                 flash build/ra6e2_fw.elf
#   .\scripts\flash.ps1 --verify        flash + read-back verify
#   .\scripts\flash.ps1 --check-only    safety check only, do not flash
# Safety gate (in the .sh): ELFs containing option-setting sections are
# refused unless --allow-ofs is passed.
& (Join-Path $PSScriptRoot 'run-sh.ps1') 'flash.sh' @args
exit $LASTEXITCODE
