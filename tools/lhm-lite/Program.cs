// lhm-lite — headless LibreHardwareMonitor-compatible data source.
//
// 仅链接未修改的 LibreHardwareMonitorLib（MPL-2.0），自写极简 host：
//   - 只开 CPU / GPU / 内存 / 主板（SuperIO 风扇必需）四类硬件
//   - 周期刷新传感器
//   - 在 127.0.0.1:8085/data.json 输出与原版 LHM 同结构的 JSON 树
// 让 ra6e2-monitor 的 collector.rs 零改动即可对接。
//
// 需要管理员权限：底层 PawnIO 内核驱动加载需要提权（与原版 LHM 一致）。

using System;
using System.Threading;
using System.Threading.Tasks;

namespace LhmLite
{
    internal static class Program
    {
        // 单实例互斥，避免重复拉起抢占 8085 端口。
        private const string MutexName = @"Global\Ra6e2MonitorLhmLite";

        private static readonly CancellationTokenSource Cts = new CancellationTokenSource();

        private static int Main(string[] args)
        {
            int port = 8085;
            for (int i = 0; i < args.Length; i++)
            {
                var lower = args[i].ToLowerInvariant();
                if (lower == "--port" && i + 1 < args.Length && int.TryParse(args[i + 1], out var pSpace))
                {
                    port = pSpace;
                    i++; // 已消费掉端口号参数
                }
                else if (lower.StartsWith("--port=") && int.TryParse(args[i].Substring(7), out var pEq))
                {
                    port = pEq;
                }
                // 兼容原 autostart 任务的 /minimized 参数（headless 实际无窗口，忽略即可）
            }

            using (var mutex = new Mutex(true, MutexName, out bool created))
            {
                if (!created)
                {
                    Console.Error.WriteLine("lhm-lite: 已在运行，退出。");
                    return 1;
                }

                Console.WriteLine($"lhm-lite: 启动，监听 http://127.0.0.1:{port}/data.json");

                var svc = new HardwareService();
                if (!svc.Start())
                {
                    Console.Error.WriteLine("lhm-lite: 硬件初始化失败 -> " + svc.LastError);
                    Console.Error.WriteLine("          请确认以管理员权限运行（PawnIO 内核驱动需要提权）。");
                    return 2;
                }
                Console.WriteLine("lhm-lite: 硬件已就绪。");

                var server = new HttpServer(port, svc);
                try
                {
                    server.Start();
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("lhm-lite: HTTP 服务启动失败 -> " + ex.Message);
                    svc.Stop();
                    return 3;
                }

                Console.WriteLine("lhm-lite: 已就绪。Ctrl+C 退出。");

                Console.CancelKeyPress += (_, e) =>
                {
                    e.Cancel = true;
                    Cts.Cancel();
                };

                try
                {
                    Task.Delay(Timeout.Infinite, Cts.Token).Wait(Cts.Token);
                }
                catch (OperationCanceledException)
                {
                    // 正常退出
                }

                server.Stop();
                svc.Stop();
                Console.WriteLine("lhm-lite: 已停止。");
                return 0;
            }
        }
    }
}
