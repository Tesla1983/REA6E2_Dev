# 上位机 GPU 取数规划（AMD / NVIDIA）

> 状态：**Phase 0 已完成**（2026-10-25），Phase 1+ 待开工。
> 适用代码：`gui/`（Rust + eframe/egui，内嵌 LibreHardwareMonitor）。
> 关联：`.workbuddy/memory/NOTES-host-sysmon.md`（LHM/PDH 踩坑记录）、
> `src/uart/bsp_uart_link.c`（协议解析）。

---

## 1. 目标

上位机要能对 **Intel / NVIDIA / AMD** 三家显卡统一取数：
温度、核心利用率、风扇、显存占用、功耗；任一字段拿不到就发「缺项」（固件显示 `--`），
**绝不用 0 冒充**。多卡机器（核显 + 独显、双独显）要能选卡。

---

## 2. 现状（2026-10-25 核实）

| 环节 | 实现 | 问题 |
| --- | --- | --- |
| GPU 温度 / 风扇 | 解析 LHM `data.json`，按关键词 `arc/geforce/radeon/nvidia` 命中**第一台** | 核显 + 独显或多卡时可能挑错卡 |
| GPU 利用率 | PDH `\GPU Engine(*)\Utilization Percentage`，只取 `engtype_3D` | ✅ 已修（见 §3.1） |
| 协议 | `$CT:48,CU:12,CF:1050,GT:62,GU:34,GF:1120,MU:69*XX` | 固件 `store_field()` 忽略未知 key ⇒ **可向后兼容加字段** |
| 风扇语义 | 固件当 RPM 用：`fan_advance()` 钳 0..3000，转速 = 值/24×1.5 °/s | ✅ 已定约定（见 §3.2） |

固件侧约束（改协议前必须知道）：

- 每段必须是 **2 字符 key + `:` + 值**（`bsp_uart_link.c` 的 `store_field()` 按 `key[0]/key[1]` 分派）。
- 未知 key **静默忽略**，所以上位机可以先发新字段，老固件不会报错。
- 数值为 `int16`，缺项 = `-1`。

---

## 3. 本轮已实现（Phase 0 附带）

### 3.1 PDH 使用率按 LUID 过滤（修 bug）

原实现把所有适配器的 `engtype_3D` 实例**一起相加**：核显 + 独显机器虚高，
双卡机器直接是错的（本机实测同一时刻有 **406** 个实例，其中属于 A770 的只有 **21** 个）。

修法（`gui/src/collector.rs`）：

1. 新增 `AdapterInfo` + `enumerate_adapters()`（DXGI `IDXGIFactory1::EnumAdapters1` + `GetDesc1`）；
2. 新增 `pick_primary_adapter()`：跳过 `DXGI_ADAPTER_FLAG_SOFTWARE`，取**显存最大**的那张；
3. `GpuUsage` 记住目标卡的 LUID 片段 `luid_0x<hi8>_0x<lo8>`（实测小写、8 位、先 high 后 low）；
   `sample()` 先过 LUID 再过 `engtype_3d`；
4. `last_stats()` 暴露「命中实例数 / 全部实例数」，供探针与排障。

实测（本机 Intel Arc A770）：

```
[0] Intel(R) Arc(TM) A770 Graphics   vendor=0x8086 vram=15.9GiB
    luid=luid_0x00000000_0x00011ce4 software=false
[1] Microsoft Basic Render Driver    vendor=0x1414 vram=0.0GiB   software=true
sample=Some(0)  matched=21/406          ← 过滤生效
```

### 3.2 风扇单位约定（RPM 优先，百分比 ×20）

不是每家都能给 RPM：主板风扇（SuperIO）、多数 A 卡（ADL）有 RPM；
**NVIDIA 桌面卡经 NVML 多数只给百分比**。

约定：

| 来源 | 上线值 | UI 后缀 |
| --- | --- | --- |
| 真实 RPM | 原值（钳 0..3000，与固件一致） | 空 |
| 只有百分比 | `round(% × 20)`，范围 0..2000 | `%` |

理由：100% ⇒ 2000，落在固件 3000 钳位之内且转速观感合理；界面标 `%` 不骗人。

⚠ **协议值 ≠ 显示值**（2026-10-25 踩过）：`Sample.cf/gf` 是协议值（百分比已 ×20），
界面要用 `Sample.cf_display/gf_display`（`FanReading::display()`：RPM 原值 / 百分比 0..100）。
否则 13% 会显示成 "260%"。

⚠ **同名叶子陷阱**：LHM 里 `Fans/System Fan #3 = 713 RPM` 与
`Controls/System Fan #3 = 19.0 %` 是同一个物理风扇的两个通道。
`Controls` 是 PWM 占空比，**不能当转速**。解析必须**RPM 优先**、百分比仅作兜底
（`lhm_parse()` 里分 `*_rpm` / `*_pct` 两路收集，最后 `rpm.or(pct)`）。

实现在 `collector.rs` 的 `FanUnit` / `FanReading`：LHM 叶子值自带单位（`1050 RPM` / `60 %`），
按后缀判定，不再假设「一定是 RPM」。`Sample.cf_unit / gf_unit` 带到 UI。

> ⚠ 后续 NVML / ADL 后端必须走同一个 `FanReading`，**不要各自直接塞原始数字**。

---

## 4. 数据源选择（目标形态）

| 字段 | NVIDIA | AMD | Intel（现有） | 通用兜底 |
| --- | --- | --- | --- | --- |
| 温度 | NVML `GetTemperature` | ADL `OD6/OD8_Temperature_Get` | IGCL / LHM | LHM |
| 利用率 | NVML `GetUtilizationRates` | ADL `CurrentActivity` | PDH（已按 LUID） | PDH |
| 风扇 | NVML `GetFanSpeed_v2`（%） | ADL `FanSpeed_Get`（% 或 RPM） | LHM RPM | LHM |
| 显存 | `nvmlDeviceGetMemoryInfo` | ADL / DXGI | DXGI | DXGI `QueryVideoMemoryInfo` |
| 功耗 | `nvmlDeviceGetPowerUsage` | ADL OD | LHM | LHM |

关键事实：

- **NVML / ADL 不需要管理员**（只有 LHM 读 MSR/SuperIO 要），本程序已 `requireAdministrator`，
  两个库都能直接 `LoadLibrary`。
- 跨库对卡用 **PCI BDF** 做主键（NVML `nvmlDeviceGetHandleByPciBusId_v2`、
  ADL `ADL_Adapter_PciBusNumber`），LUID 只用于 DXGI/PDH，索引只用于厂商库内部。

---

## 5. 分阶段计划

### Phase 1 · 抽象层（不碰 UI、不碰协议）

新增 `gui/src/gpu/mod.rs`：

```rust
pub struct AdapterKey { vendor: Vendor, pci_bdf: PciBdf, luid: (u32, u32) }
pub struct GpuSample {
    pub temp_c: Option<i32>, pub util_pct: Option<i32>,
    pub fan: Option<FanReading>, pub mem_used_mb: Option<u32>,
    pub mem_total_mb: Option<u32>, pub power_w: Option<f32>,
    pub source: Source,                      // Nvml / Adl / Lhm / Pdh / Igl
}
pub trait GpuBackend {
    fn caps(&self) -> Caps;                  // 能力位，UI/日志用
    fn enumerate(&self) -> Vec<AdapterKey>;
    fn read(&self, key: &AdapterKey) -> Option<GpuSample>;
}
```

`Resolver`：合并各后端 → 按能力位补齐 → 选卡（默认显存最大的独显，允许 UI 覆盖）。

**验收**：只用 LHM + PDH 能 100% 复现当前 A770 的数值（零回归）。

### Phase 2 · NVML 后端（`gpu/nvml.rs`）

`libloading` 动态加载 `nvml.dll`；加载失败即视为「无 NVIDIA」。
`nvmlInit_v2` → `nvmlDeviceGetCount_v2` → `GetHandleByPciBusId_v2` → 各项 Get。

**验收**：N 卡机器温度/利用率/风扇/功耗/显存全出；驱动重启后能自动重建句柄。
风扇走 `FanReading::percent`（%×20）。

### Phase 3 · ADL 后端（`gpu/adl.rs`，手写 FFI）

`ADL_Main_Control_Create` → `ADL_Adapter_AdapterInfo_Get` → `ADL_Adapter_PciBusNumber` 对齐 BDF
→ `OD6/OD8_Temperature_Get`、`OD6_FanSpeed_Get`、`OD6_CurrentActivity_Get`。

**验收**：A 卡机器温度与 AMD 驱动面板误差 ≤1 ℃；OD6 失效时（新 RDNA）自动降级到 LHM/PDH 并在 UI 标来源。

### Phase 4 · LHM 选卡修正 + 面板

- LHM 兜底改为**按选中卡的名字匹配**（不再是"第一个命中的关键词"），多卡时用下拉选卡；
- UI 增加「数据来源」标签（NVML / ADL / LHM / PDH）与选卡下拉。

**验收**：iGPU + dGPU 机器上独显数值与任务管理器一致。

### Phase 5 · 协议扩展（固件暂不必改）

固件 key 必须 2 字符，建议：

| key | 含义 |
| --- | --- |
| `T2` `U2` `F2` | 第二块 GPU 温度 / 利用率 / 风扇 |
| `GV` | 显存占用 % |
| `GP` | 功耗 W |

固件 v24 会忽略这些 key，可以先发；等需要屏上显示双卡时再改固件版面
（4 行已排满：建议把 `RAM` 行改成 `RAM / VRAM` 双列，或用「两卡合一」单元格）。

---

## 6. 测试与风险

### 本机的现实限制

本机只有 **Intel Arc A770**，`System32` 下 **没有 `nvml.dll` / `atiadlxx.dll` / `amdadlx64.dll`**
（`igcl.dll` 也没有，现有 Intel 路径靠 LHM）⇒ **NVIDIA / AMD 代码无法在本机真机验证**。

对策：

1. `MockBackend` + 录制的 JSON 做单测；
2. 在真有 N 卡 / A 卡的机器上跑 `ra6e2-monitor.exe --probe-gpu`，把输出贴回；
3. Phase 0 探针已能一眼看出：DXGI 有没有枚举到卡、LUID 是多少、PDH 命中多少实例、
   厂商 DLL 在不在、LHM 里这张卡叫什么。

### 探针用法

```bat
:: 需要管理员（程序清单是 requireAdministrator）
ra6e2-monitor.exe --probe-gpu
:: 输出同时写 %TEMP%\ra6e2-gpu-probe.txt
```

### 单元测试

```bat
:: 测试二进制若嵌了提权清单就跑不起来，用 build.rs 现成的开关绕开
set RA6E2_NO_UAC=1 && cargo test --release
:: ⚠ 跑完必须不带该变量再 cargo build --release，否则交付的 exe 没有提权清单
```

### 风险清单

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| ADL OD6 在新 RDNA 上部分失效 | 温度/风扇取不到 | 降级 LHM/PDH，UI 标来源 |
| NVAPI 私有风扇 RPM 接口不稳 | 拿到错值 | **只用 NVML 官方接口**，风扇给 % |
| LHM 需要管理员 | 非提权时 CPU 温度/风扇全无 | 程序保持 `requireAdministrator` |
| 厂商 DLL 调用阻塞 | UI 卡顿 | 一律在采集线程，不碰 UI 线程 |
| 热插拔 / 驱动重启 | 句柄失效 | 每次失败重建后端句柄，不缓存指针 |
| 多卡索引顺序变化 | 取错卡 | 只用 PCI BDF / LUID，禁用「索引」跨库匹配 |

---

## 7. 落地顺序建议

1. Phase 1 抽象层（半天，零行为变化）；
2. Phase 2 NVML（一天，本机可用 `LoadLibrary` 失败路径 + Mock 验证）；
3. Phase 3 ADL（一天，FFI 最费事）；
4. 拿到真机探针输出后再定风扇/显示的细节；
5. Phase 4 / 5 按需（不强依赖前面完成）。
