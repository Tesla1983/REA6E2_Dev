// HardwareService：封装 LibreHardwareMonitorLib.Computer 的初始化与周期刷新。
//
// 只开采集必需的硬件组；关掉 Storage/Network/PSU/Battery/PowerMonitor，
// 把原版 LHM 的「全硬件枚举 + 磁盘 SMART + 网卡」纯开销砍掉。
// 刷新只调用 hardware.Update()（不在 HTTP 请求里阻塞枚举），请求直接读已刷新的 Value。

using System;
using System.Collections.Generic;
using System.Threading;
using LibreHardwareMonitor.Hardware;

namespace LhmLite
{
    internal sealed class HardwareService
    {
        private Computer _computer;
        private readonly Timer _timer;

        public string LastError { get; private set; } = "";

        public HardwareService()
        {
            // 250ms 周期（原 1s）；初始不调度，Start() 里手动触发一次。
            // 硬件 Update 只是读寄存器/SuperIO，250ms 开销很小，换来 UI 更实时。
            _timer = new Timer(_ => Refresh(), null, Timeout.Infinite, Timeout.Infinite);
        }

        public bool Start()
        {
            try
            {
                _computer = new Computer
                {
                    IsCpuEnabled = true,
                    IsGpuEnabled = true,
                    IsMemoryEnabled = true,
                    IsMotherboardEnabled = true,   // SuperIO(NCT6687D) 风扇必需
                    IsControllerEnabled = true,    // 部分板载 SuperIO 走此路径
                    IsStorageEnabled = false,      // 省磁盘 SMART 枚举
                    IsNetworkEnabled = false,      // 省网卡枚举
                    IsPsuEnabled = false,
                    IsBatteryEnabled = false,
                    IsPowerMonitorEnabled = false,
                };
                _computer.Open();
                Refresh();               // 首刷，让 Value 立即可用
                _timer.Change(0, 250);   // 之后每 250ms 刷新（配合 GUI 250ms 采样）
                return true;
            }
            catch (Exception ex)
            {
                LastError = ex.GetType().Name + ": " + ex.Message;
                return false;
            }
        }

        private void Refresh()
        {
            var computer = _computer;
            if (computer == null)
                return;
            try
            {
                foreach (var hw in computer.Hardware)
                {
                    hw.Update();
                    foreach (var sub in hw.SubHardware)
                        sub.Update();
                }
            }
            catch
            {
                // 热插拔/驱动瞬时失效：降级为空数据，下一周期再试，不崩溃。
            }
        }

        /// <summary>当前枚举到的全部硬件（含子硬件由 JsonTree 自行展开 SubHardware）。</summary>
        public IEnumerable<IHardware> Hardware
        {
            get
            {
                if (_computer == null)
                    yield break;
                foreach (var hw in _computer.Hardware)
                    yield return hw;
            }
        }

        public void Stop()
        {
            try { _timer.Change(Timeout.Infinite, Timeout.Infinite); } catch { }
            try { _computer?.Close(); } catch { }
            _computer = null;
        }
    }
}
