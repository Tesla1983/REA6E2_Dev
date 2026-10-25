#Requires -Version 5.1
<#
自动测量「板子重启 → COM 口可用」的耗时, **不需要人工拔插**。

原理: `probe-rs reset` 只复位内核, 但 MCU 复位会让 USB 断开再连上,
      主机侧表现为一次 surprise-removal + 重新枚举, 与物理拔插等效
      (实测两者得到的耗时一致, 均为 ~30s), 故可用它代替人工拔插。

用法:
    powershell -ExecutionPolicy Bypass -File E:\REA6E2_Dev\scripts\autotest_com_delay.ps1
    .\scripts\autotest_com_delay.ps1 -Probe 0416:5021 -Rounds 2     # 连测 2 轮

输出三个口径: PnP devnode / 注册表 SERIALCOMM / WMI(对照)。
#>
param(
    [string] $Probe         = '0416:5021',
    [string] $Vid           = 'VID_045B',
    [int]    $WaitGoneSec   = 20,
    [int]    $WaitPresentSec= 40,
    [int]    $WaitComSec    = 45,
    [int]    $Rounds        = 1
)

$ErrorActionPreference = 'SilentlyContinue'
$pr = 'E:\.cargo\bin\probe-rs.exe'

function Get-Dev {
    return (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*$Vid*" } | Select-Object -First 1)
}
function Get-ComFromPnp {
    $d = Get-Dev
    if ($d -and $d.FriendlyName -match '(COM\d+)') { return $Matches[1] }
    return $null
}
function Test-ComInReg([string]$c) {
    if (-not $c) { return $false }
    $k = Get-ItemProperty 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM'
    if (-not $k) { return $false }
    $vals = @($k.PSObject.Properties | Where-Object { $_.Name -like '\Device*' } | ForEach-Object { $_.Value })
    return ($vals -contains $c)
}
function Test-ComInWmi([string]$c) {
    if (-not $c) { return $false }
    $w = Get-CimInstance Win32_SerialPort | Where-Object { $_.DeviceID -eq $c } | Select-Object -First 1
    return ($null -ne $w)
}

$com = Get-ComFromPnp
if (-not $com) {
    # 烧录后内核停在 halt, USB 未枚举 ⇒ 先 reset 唤醒它, 再开始正式测量
    Write-Host "设备当前不在位(烧录后 halt), 先 reset 唤醒 ..."
    & $pr reset --chip R7FA6E2BB --probe $Probe --speed 1000 2>&1 | Select-Object -Last 1 | ForEach-Object { "    $_" }
    $wake = Get-Date
    while (((Get-Date) - $wake).TotalSeconds -lt 45) {
        if (Get-Dev) { break }
        Start-Sleep -Milliseconds 300
    }
    Start-Sleep -Seconds 2
    $com = Get-ComFromPnp
    if (-not $com) {
        Write-Host "✘ 唤醒后设备仍未出现($Vid)。检查接线/供电。"
        exit 1
    }
    Write-Host ("已唤醒: {0}  (等待 {1:N1}s)" -f $com, ((Get-Date) - $wake).TotalSeconds)
}
Write-Host "当前端口: $com   SERIALCOMM 已登记: $(Test-ComInReg $com)"
Write-Host ""

for ($round = 1; $round -le $Rounds; $round++) {
    Write-Host "===== 第 $round / $Rounds 轮 ====="
    Write-Host ">>> 触发 probe-rs reset (等效板子重启) ..."
    & $pr reset --chip R7FA6E2BB --probe $Probe --speed 1000 2>&1 | Select-Object -Last 2 | ForEach-Object { "    $_" }
    $tReset = Get-Date

    # 1) 等设备消失
    $tGone = $null
    while (((Get-Date) - $tReset).TotalSeconds -lt $WaitGoneSec) {
        if (-not (Get-Dev)) { $tGone = Get-Date; break }
        Start-Sleep -Milliseconds 200
    }
    if (-not $tGone) { Write-Host "⚠ 设备没消失(复位可能没让 USB 掉线), 继续等它重新出现"; $tGone = $tReset }
    Write-Host ("    设备消失 @ {0:HH:mm:ss.fff}" -f $tGone)

    # 2) 等 PnP devnode 重新出现
    $tPnp = $null
    while (((Get-Date) - $tGone).TotalSeconds -lt $WaitPresentSec) {
        if (Get-Dev) { $tPnp = Get-Date; break }
        Start-Sleep -Milliseconds 150
    }
    if (-not $tPnp) { Write-Host "✘ 等待设备重新出现超时"; continue }
    $com = Get-ComFromPnp
    Write-Host ("    设备出现 @ {0:HH:mm:ss.fff}  端口={1}  (距消失 {2:N2}s)" -f $tPnp, $com, ((Get-Date) - $tGone).TotalSeconds)

    # 3) 等 SERIALCOMM / WMI
    $tReg = $null; $tWmi = $null
    $nextLog = 0
    while (((Get-Date) - $tPnp).TotalSeconds -lt $WaitComSec) {
        $el = ((Get-Date) - $tPnp).TotalSeconds
        if (-not $tReg -and (Test-ComInReg $com)) { $tReg = Get-Date }
        if (-not $tWmi -and (Test-ComInWmi $com)) { $tWmi = Get-Date }
        if ($el -ge $nextLog) {
            Write-Host ("    t+{0,5:N1}s  SERIALCOMM={1}  WMI={2}" -f $el, [bool]$tReg, [bool]$tWmi)
            $nextLog += 4
        }
        if ($tReg -and $tWmi) { break }
        Start-Sleep -Milliseconds 200
    }

    Write-Host "    ---------------------"
    if ($tPnp) { Write-Host ("    COM(PnP devnode)     : {0}  {1,5:N2} 秒" -f $com, (($tPnp - $tGone).TotalSeconds)) }
    if ($tReg) { Write-Host ("    COM(注册表SERIALCOMM): {0}  {1,5:N2} 秒" -f $com, (($tReg - $tPnp).TotalSeconds)) } else { Write-Host "    COM(注册表SERIALCOMM): ✘ 未在 $WaitComSec 秒内出现" }
    if ($tWmi) { Write-Host ("    COM(WMI,对照)        : {0}  {1,5:N2} 秒" -f $com, (($tWmi - $tPnp).TotalSeconds)) } else { Write-Host "    COM(WMI,对照)        : ✘ 未出现" }
    Write-Host ""

    if ($round -lt $Rounds) { Start-Sleep -Seconds 3 }
}
