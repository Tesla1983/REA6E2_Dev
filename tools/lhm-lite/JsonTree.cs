// JsonTree：把 LibreHardwareMonitorLib 的硬件树转成与原版 LHM /data.json 同结构的 JSON 字符串。
//
// 层级（与 ra6e2-monitor/collector.rs 的 walk()/hw_name() 契约严格对齐）：
//   Sensor
//     └─ <机器名>                         (Environment.MachineName)
//          └─ <硬件名>                    (IHardware.Name，如 "Intel Core i7..." / "Nuvoton NCT6687D" / "Intel Arc A770 Graphics")
//               └─ Temperatures / Fans / Controls   (TypeNode 分组名，精确匹配 collector 白名单)
//                    └─ <传感器名> : "Value"         (带单位后缀，collector 按后缀判定 °c/rpm/%)
//
// 只输出 Temperature / Fan / Control 三类（上位机只用这几类）。其余类型跳过，缩小体积。

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using LibreHardwareMonitor.Hardware;

namespace LhmLite
{
    internal static class JsonTree
    {
        public static string Build(IEnumerable<IHardware> hardware)
        {
            var sb = new StringBuilder();
            sb.Append("{\"Text\":\"Sensor\",\"Children\":[");
            sb.Append("{\"Text\":");
            AppendJsonString(sb, Environment.MachineName);
            sb.Append(",\"Children\":[");
            bool first = true;
            foreach (var hw in hardware)
            {
                if (!first) sb.Append(',');
                first = false;
                sb.Append(HardwareNode(hw));
            }
            sb.Append("]}"); // 关机器节点
            sb.Append("]}"); // 关 Sensor 根
            return sb.ToString();
        }

        private static string HardwareNode(IHardware hw)
        {
            var children = new List<string>();

            AddGroup(children, "Temperatures", hw, SensorType.Temperature,
                v => v.ToString("F1", CultureInfo.InvariantCulture) + " °C");
            AddGroup(children, "Fans", hw, SensorType.Fan,
                v => v.ToString("F0", CultureInfo.InvariantCulture) + " RPM");
            AddGroup(children, "Controls", hw, SensorType.Control,
                v => v.ToString("F1", CultureInfo.InvariantCulture) + " %");

            // 子硬件各自成节点（NCT6687D 风扇常在主板 SuperIO 子硬件下），
            // 让 hw_name() 取到子硬件名（含 nct → is_superio 命中）。
            foreach (var sub in hw.SubHardware)
                children.Add(HardwareNode(sub));

            var sb = new StringBuilder();
            sb.Append("{\"Text\":");
            AppendJsonString(sb, hw.Name);
            sb.Append(",\"Children\":[");
            sb.Append(string.Join(",", children));
            sb.Append("]}");
            return sb.ToString();
        }

        private static void AddGroup(List<string> outGroups, string groupName, IHardware hw,
            SensorType type, Func<float, string> format)
        {
            var leaves = new List<string>();
            Collect(leaves, hw.Sensors, type, format);
            if (leaves.Count == 0)
                return;

            var g = new StringBuilder();
            g.Append("{\"Text\":");
            AppendJsonString(g, groupName);
            g.Append(",\"Children\":[");
            g.Append(string.Join(",", leaves));
            g.Append("]}");
            outGroups.Add(g.ToString());
        }

        private static void Collect(List<string> leaves, IEnumerable<ISensor> sensors,
            SensorType type, Func<float, string> format)
        {
            foreach (var s in sensors)
            {
                if (s.SensorType != type)
                    continue;
                if (!s.Value.HasValue)
                    continue;
                var leaf = new StringBuilder();
                leaf.Append("{\"Text\":");
                AppendJsonString(leaf, s.Name);
                leaf.Append(",\"Value\":");
                AppendJsonString(leaf, format(s.Value.Value));
                leaf.Append("}");
                leaves.Add(leaf.ToString());
            }
        }

        /// <summary>最小 JSON 字符串转义（不含外层引号，调用方负责加）。</summary>
        private static void AppendJsonString(StringBuilder sb, string s)
        {
            sb.Append('"');
            if (s != null)
            {
                foreach (var c in s)
                {
                    switch (c)
                    {
                        case '\\': sb.Append("\\\\"); break;
                        case '"': sb.Append("\\\""); break;
                        case '\b': sb.Append("\\b"); break;
                        case '\f': sb.Append("\\f"); break;
                        case '\n': sb.Append("\\n"); break;
                        case '\r': sb.Append("\\r"); break;
                        case '\t': sb.Append("\\t"); break;
                        default:
                            if (c < 0x20)
                                sb.Append("\\u").Append(((int)c).ToString("x4"));
                            else
                                sb.Append(c);
                            break;
                    }
                }
            }
            sb.Append('"');
        }
    }
}
