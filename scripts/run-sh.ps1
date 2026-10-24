#!/usr/bin/env pwsh
# ------------------------------------------------------------------
# run-sh.ps1 - run scripts/*.sh through Git Bash on Windows
#
#   .\scripts\run-sh.ps1 rtt.sh                 -> ./scripts/rtt.sh
#   .\scripts\run-sh.ps1 flash.sh --verify
#   .\scripts\run-sh.ps1 build.sh
#
# Why this exists:
#   Typing  E:\REA6E2_Dev\scripts\rtt.sh  in PowerShell does NOT run it.
#   For a non-executable extension PowerShell falls back to the Windows
#   shell "open" verb, and on this machine .sh is associated with Code.exe,
#   so VS Code just opens the file as text and prints its own startup log.
#   A .sh file must be fed to bash.exe.
#
# Notes:
#   - Always runs with cwd = project root, because the .sh scripts resolve
#     build/ra6e2_fw.elf relative to the project root.
#   - Backslashes in arguments are converted to forward slashes for bash.
#   - Exit code of the shell script is propagated.
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = 'Stop'

if (-not $Rest -or $Rest.Count -lt 1) {
    Write-Host "usage: .\scripts\run-sh.ps1 <script.sh> [args...]"
    Write-Host "e.g.   .\scripts\run-sh.ps1 rtt.sh"
    Write-Host "       .\scripts\run-sh.ps1 flash.sh --verify"
    exit 1
}

$ScriptName = $Rest[0]
$PassArgs   = if ($Rest.Count -gt 1) { $Rest[1..($Rest.Count - 1)] } else { @() }

# --- locate bash.exe (Git for Windows) ---
$Bash = $null
foreach ($c in @(
    'C:\Program Files\Git\bin\bash.exe',
    'C:\Program Files\Git\usr\bin\bash.exe',
    'C:\Program Files (x86)\Git\bin\bash.exe'
)) {
    if (Test-Path -LiteralPath $c) { $Bash = $c; break }
}
if (-not $Bash) {
    $found = Get-Command bash.exe -ErrorAction SilentlyContinue
    if ($found) { $Bash = $found.Source }
}
if (-not $Bash) {
    Write-Host "ERROR: bash.exe not found. Install Git for Windows, or add it to PATH." -ForegroundColor Red
    exit 1
}

# --- resolve paths ---
$Root = Split-Path -Parent $PSScriptRoot
$Sh   = Join-Path (Join-Path $Root 'scripts') $ScriptName
if (-not (Test-Path -LiteralPath $Sh)) {
    Write-Host "ERROR: script not found: $Sh" -ForegroundColor Red
    exit 1
}

function To-Posix([string]$p) {
    $p = $p -replace '\\', '/'
    if ($p -match '^([A-Za-z]):/(.*)$') {
        $p = '/' + $Matches[1].ToLower() + '/' + $Matches[2]
    }
    return $p
}

$posixRoot = To-Posix $Root
$posixSh   = To-Posix $Sh

$quoted = @()
foreach ($a in $PassArgs) {
    $q = "'" + ($a -replace "'", "'\\''") + "'"
    $quoted += $q
}
$argStr = ($quoted -join ' ')

$cmd = "cd '$posixRoot' && bash '$posixSh'"
if ($argStr) { $cmd += " $argStr" }

Write-Host ">>> bash: $cmd" -ForegroundColor DarkGray

# --- 中文乱码修复 -------------------------------------------------------
# bash 输出的是 UTF-8，而 Windows 控制台默认按 GBK(936) 解码，
# 不处理会显示成 ">>> 鐩爣: R7FA6E2BB   鎺㈤拡: 0d28:0204" 这种乱码。
# 调用前临时把控制台输出编码切成 UTF-8，用完恢复，不影响后续命令。
# ⚠ 实测坑（VSCode 集成终端）: [Console]::OutputEncoding 的**读取**可能直接返回 $null,
#   于是 finally 里用它恢复时炸出:
#       设置"OutputEncoding"时发生异常:"值不能为 null。参数名: value"
#   本脚本开头设了 $ErrorActionPreference='Stop', .NET 异常会直接终止脚本。
#   ⇒ 读、写、恢复三步全部 try/catch 包死, 失败就放弃调色, 绝不让它炸。
$prevEnc = $null
try { $prevEnc = [Console]::OutputEncoding } catch { $prevEnc = $null }

try {
    try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
    & $Bash -lc $cmd
    $code = $LASTEXITCODE
}
finally {
    try {
        if ($null -ne $prevEnc) { [Console]::OutputEncoding = $prevEnc }
    } catch { }
}

exit $code
