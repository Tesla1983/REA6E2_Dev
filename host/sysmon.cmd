@echo off
REM ============================================================
REM  sysmon launcher - run from anywhere, no cwd juggling needed.
REM  Picks the Python that has pywin32+psutil (fast COM path) first.
REM  Usage:
REM      sysmon.cmd                 full report
REM      sysmon.cmd --line          one compact line (for serial)
REM      sysmon.cmd --providers     show which data sources are alive
REM      sysmon.cmd --selftest      self test
REM      sysmon.cmd --loop 5        loop 5 times
REM ============================================================
setlocal

set "PY="
if exist "%USERPROFILE%\.workbuddy\binaries\python\envs\default\Scripts\python.exe" (
    set "PY=%USERPROFILE%\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
)

if not defined PY (
    for %%P in (python.exe) do (
        if not defined PY set "PY=%%~$PATH:P"
    )
)

if not defined PY (
    echo [ERROR] No python.exe found in PATH.
    echo         Install Python 3, or run with a full python path.
    pause
    exit /b 1
)

"%PY%" "%~dp0sysmon.py" %*
set "RC=%ERRORLEVEL%"

REM Intentionally no pause: it would hang when called from PowerShell with no
REM arguments. If you double-click this file the window closes at the end --
REM use PowerShell (or --loop N) instead of double-clicking.

endlocal & exit /b %RC%
