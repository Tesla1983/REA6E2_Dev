// HttpServer：极简 HTTP 服务，仅在 127.0.0.1:<port> 提供 GET /data.json。
//
// 用 TcpListener 而非 HttpListener：避免 http.sys URL ACL 依赖（原版 LHM 需管理员注册，
// 这里裸 socket 绑定回环地址即可，权限只受 PawnIO 驱动约束）。collector.rs 每次采样新建连接，
// 因此「一连接一响应即关闭」足够，无需 keep-alive。

using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using LibreHardwareMonitor.Hardware;

namespace LhmLite
{
    internal sealed class HttpServer
    {
        private readonly int _port;
        private readonly HardwareService _svc;
        private TcpListener _listener;
        private Thread _acceptThread;
        private volatile bool _running;

        public HttpServer(int port, HardwareService svc)
        {
            _port = port;
            _svc = svc;
        }

        public void Start()
        {
            _listener = new TcpListener(IPAddress.Loopback, _port);
            _listener.Start();
            _running = true;
            _acceptThread = new Thread(AcceptLoop) { IsBackground = true, Name = "lhm-http" };
            _acceptThread.Start();
        }

        private void AcceptLoop()
        {
            while (_running)
            {
                TcpClient client;
                try
                {
                    client = _listener.AcceptTcpClient();
                }
                catch (SocketException)
                {
                    break; // Stop() 调用 listener.Stop() 会落到这里
                }
                catch (ObjectDisposedException)
                {
                    break;
                }

                if (client == null)
                    continue;

                ThreadPool.QueueUserWorkItem(HandleClient, client);
            }
        }

        private void HandleClient(object state)
        {
            var client = (TcpClient)state;
            try
            {
                using (client)
                using (var ns = client.GetStream())
                {
                    var buf = new byte[2048];
                    int n = ns.Read(buf, 0, buf.Length);
                    if (n <= 0)
                        return;

                    // 取请求行里的 path（"GET /data.json HTTP/1.1"）
                    var requestLine = Encoding.ASCII.GetString(buf, 0, n);
                    int firstSpace = requestLine.IndexOf(' ');
                    int secondSpace = firstSpace >= 0 ? requestLine.IndexOf(' ', firstSpace + 1) : -1;
                    string path = "/";
                    if (firstSpace >= 0 && secondSpace > firstSpace)
                        path = requestLine.Substring(firstSpace + 1, secondSpace - firstSpace - 1);

                    string body;
                    int code;
                    string reason;
                    if (path == "/data.json")
                    {
                        body = JsonTree.Build(_svc.Hardware);
                        code = 200;
                        reason = "OK";
                    }
                    else
                    {
                        body = "{\"error\":\"not found\"}";
                        code = 404;
                        reason = "Not Found";
                    }

                    var header = string.Format(
                        "HTTP/1.1 {0} {1}\r\n" +
                        "Content-Type: application/json\r\n" +
                        "Content-Length: {2}\r\n" +
                        "Cache-Control: no-cache\r\n" +
                        "Access-Control-Allow-Origin: *\r\n" +
                        "Connection: close\r\n\r\n",
                        code, reason, Encoding.UTF8.GetByteCount(body));

                    var outBytes = Encoding.UTF8.GetBytes(header + body);
                    ns.Write(outBytes, 0, outBytes.Length);
                }
            }
            catch
            {
                // 客户端提前断开等：忽略，不崩溃
            }
        }

        public void Stop()
        {
            _running = false;
            try { _listener?.Stop(); } catch { }
        }
    }
}
