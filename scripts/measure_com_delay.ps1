# =============================================================================
# RA6E2 CDC —— 测量"设备枚举完成 -> COM 口可用"的耗时
#
# 用法(普通 PowerShell 即可, 不需要管理员):
#   powershell -ExecutionPolicy Bypass -File scripts\measure_com_delay.ps1
#
# 流程: 若设备当前已在位, 会提示先拔掉; 然后等你插回, 自动算出结果。
#
# 为什么测三个口径(这是本次调查的关键):
#   [1] PnP Ports 类        —— PnP 认为 COM 口已建立(devnode OK)
#   [2] SERIALCOMM 注册表   —— HKLM\HARDWARE\DEVICEMAP\SERIALCOMM,
#                              **扫描工具真正读的表**(pyserial / .NET GetPortNames /
#                              本项目 GUI 都靠它枚举串口)
#   [3] WMI Win32_SerialPort —— 以"慢/返回陈旧数据"著称, 仅作对照
#   2026-10-25 实测到过: PnP 里 COM17 已 OK, 但 SERIALCOMM 当时还没有 COM17,
#   过一会才补上 —— 这正对应"提示音已响但要等十几秒才**扫描到**串口"。
# =============================================================================
param(
    [int] $WaitUnplugSec = 40,   # 等"设备消失"(你拔掉)的最长时间
    [int] $WaitDeviceSec = 55,   # 等"设备出现"(你插回)的最长时间
    [int] $WaitComSec    = 90,   # 设备出现后, 等"COM 口出现"的最长时间
    [string] $VidPid = '045B',   # 要测的设备 VID(4 位十六进制)。对照组可传 1A86(CH340)
    [switch] $ProbeOpen,         # 一旦 PnP 给出 COM 号, 立刻并行尝试"提前打开"它
    [string] $PyExe = 'C:\Users\Administrator\AppData\Local\Programs\Python\Python312\python.exe'
)

$ErrorActionPreference = 'SilentlyContinue'

# 按 VID_045B 精确匹配, 不用"基线比对"。
# 原因: 基线若在设备还插着时采集, 会包含 COM17, 导致插回后检测不到"新 COM"(曾误报"未出现")。
# 已确认 PnP Ports 类里 COM17 的 InstanceId 本身就是 USB\VID_045B&PID_0263\<序列号>,
# 所以可以直接按 VID 精确定位, 与"基线里有没有"无关。
function Get-ComPnp {
    $d = Get-PnpDevice -Class Ports -PresentOnly |
         Where-Object { $_.InstanceId -like "*VID_$VidPid*" } |
         Select-Object -First 1
    if ($d -and $d.FriendlyName -match '(COM\d+)') { return $Matches[1] }
    return $null
}
# 注册表 SERIALCOMM 不带 VID, 因此按"COM 名"查: 传入 PnP 已确认的 COM 名去注册表里找。
function Test-ComInReg([string]$comName) {
    if (-not $comName) { return $false }
    $k = Get-ItemProperty 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM'
    if (-not $k) { return $false }
    $vals = @($k.PSObject.Properties | Where-Object { $_.Name -like '\Device*' } | ForEach-Object { $_.Value })
    return ($vals -contains $comName)
}
function Get-Dev {
    return (Get-PnpDevice -PresentOnly |
            Where-Object { $_.InstanceId -like "*VID_$VidPid*" } |
            Select-Object -First 1)
}

Write-Host "匹配方式: 按 InstanceId 含 VID_$VidPid 精确定位(不做基线比对)"

# ---- 阶段 0: 若设备已在位, 等它消失(你拔掉) ----
$dev = Get-Dev
if ($dev) {
    Write-Host "设备当前已在位 ($($dev.InstanceId))。"
    Write-Host ">>> 请先拔掉 Type-C，我等你拔 (最多 $WaitUnplugSec 秒)…"
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $WaitUnplugSec) {
        if (-not (Get-Dev)) { break }
        Start-Sleep -Milliseconds 200
    }
    if (Get-Dev) { Write-Host "✘ 没等到设备消失, 退出。"; exit 1 }
    Write-Host ">>> 已拔掉。现在插回 Type-C，我等你插 (最多 $WaitDeviceSec 秒)…"
} else {
    Write-Host ">>> 请插上/拔插 Type-C，我等设备出现 (最多 $WaitDeviceSec 秒)…"
}
Write-Host ""

# ---- 阶段 1: 等设备出现 ----
$t1 = Get-Date
$dev = $null
while (((Get-Date) - $t1).TotalSeconds -lt $WaitDeviceSec) {
    $dev = Get-Dev
    if ($dev) { break }
    Start-Sleep -Milliseconds 150
}
if (-not $dev) { Write-Host "✘ 等了 $WaitDeviceSec 秒也没见到设备。"; exit 1 }

$tPresent = Get-Date
Write-Host ">>> 设备出现  $($tPresent.ToString('HH:mm:ss.fff'))"
Write-Host "    状态=$($dev.Status)  错误码=$($dev.ConfigManagerErrorCode)"
Write-Host "    实例ID=$($dev.InstanceId)"
Write-Host "    正在测 COM 口出现时间…"
Write-Host ""

# ---- 阶段 2: 等 COM 口出现(三个口径分别计时) ----
$tPnp = $null; $comPnp = $null
$tReg = $null; $comReg = $null
$tWmi = $null; $comWmi = $null
$nextLog = 0
$job = $null

while (((Get-Date) - $tPresent).TotalSeconds -lt $WaitComSec) {
    if (-not $tPnp) {
        $c = Get-ComPnp
        if ($c) {
            $tPnp = Get-Date; $comPnp = $c
            if ($ProbeOpen -and -not $job) {
                $probe = Join-Path $PSScriptRoot 'probe_early_open.py'
                $job = Start-Job -ScriptBlock { param($py, $script, $name) & $py $script $name 45 } `
                                 -ArgumentList $PyExe, $probe, $c
                Write-Host "    (已并行启动『提前打开』探针: python $probe $c)"
            }
        }
    }
    if (-not $tReg) {
        # 注册表按"COM 名"查: 优先用 PnP 已确认的名字; PnP 还没出来时退化为查任意 USB 串口
        $cand = $comPnp
        if (-not $cand) {
            $k0 = Get-ItemProperty 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM'
            if ($k0) {
                $cand = @($k0.PSObject.Properties | Where-Object { $_.Name -like '\Device\USBSER*' } | ForEach-Object { $_.Value })[0]
            }
        }
        if ($cand -and (Test-ComInReg $cand)) { $tReg = Get-Date; $comReg = $cand }
    }
    if (-not $tWmi) {
        $w = Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -like "*VID_$VidPid*" } | Select-Object -First 1
        if ($w) { $tWmi = Get-Date; $comWmi = $w.DeviceID }
    }
    if ($tPnp -and $tReg -and $tWmi) { break }

    # 时间线: 每 2 秒打一行, 看这 30 秒里 devnode 到底处于什么状态
    $el = ((Get-Date) - $tPresent).TotalSeconds
    if ($el -ge $nextLog) {
        $dp = Get-PnpDevice -Class Ports -PresentOnly | Where-Object { $_.InstanceId -like "*VID_$VidPid*" } | Select-Object -First 1
        $st = if ($dp) { "$($dp.Status)/$($dp.ConfigManagerErrorCode)" } else { "(无 Ports devnode)" }
        $fn = if ($dp) { $dp.FriendlyName } else { "-" }
        Write-Host ("  t+{0,5:N1}s  Ports: {1,-28} {2,-22} SERIALCOMM={3}" -f $el, $st, $fn, (Test-ComInReg ($comPnp)))
        $nextLog += 2
    }
    Start-Sleep -Milliseconds 100
}

Write-Host "===================== 结果 ====================="
function Show-Row([string]$label, $t, $name) {
    if ($t) {
        Write-Host ("{0} : {1}   耗时 {2:N2} 秒" -f $label, $name, ($t - $tPresent).TotalSeconds)
    } else {
        Write-Host ("{0} : ✘ 设备出现后 {1} 秒内未出现" -f $label, $WaitComSec)
    }
}
Show-Row "COM(PnP,devnode建立)" $tPnp $comPnp
Show-Row "COM(注册表SERIALCOMM)" $tReg $comReg
Show-Row "COM(WMI,对照)" $tWmi $comWmi
Write-Host "==============================================="
if ($job) {
    Write-Host ""
    Write-Host "=========== 提前打开探针(能否绕过等待?) ==========="
    Receive-Job -Job $job -Wait | ForEach-Object { Write-Host $_ }
    Remove-Job -Job $job
}
Write-Host ""
if ($tPnp -and $tReg) {
    $gap = ($tReg - $tPnp).TotalSeconds
    if ($gap -gt 3) {
        Write-Host ("⚠ 关键: PnP 已认到 COM, 但『扫描工具读的那张表』晚了 {0:N1} 秒才补上。" -f $gap)
        Write-Host "  ⇒ 你原始症状『提示音已响但要等十几秒才扫描到串口』的真身可能就是这个 gap,"
        Write-Host "    它发生在**主机侧**, 装 INF 也未必能消除。"
    } else {
        Write-Host "PnP 与注册表几乎同时, 扫描延迟不是来自这里。"
    }
}
