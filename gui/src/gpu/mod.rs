//! GPU 采集抽象层（Phase 1，对应 `docs/上位机GPU取数规划_AMD_NVIDIA.md` §5）。
//!
//! 设计目标：把"按厂商取显卡数据"统一成 `GpuBackend` trait；`Resolver` 把多个后端
//! 合并、按能力位补齐字段、默认选"显存最大的非软件独显"（允许 UI 覆盖）。
//!
//! 当前后端：
//!   · `LhmPdhBackend` —— 永远在场（LHM 解析 + PDH 计数），对应改动前的 Intel 路径；
//!   · `NvmlBackend`   —— 仅当 `nvml.dll` 存在时注册（Phase 2）；
//!   · `AdlBackend`    —— 仅当 `atiadlxx.dll` 存在时注册（Phase 3）。
//!
//! 本机（只有 Intel Arc A770、无 N/A 厂商 DLL）只会用 `LhmPdhBackend`，行为与改动前
//! **完全一致**（零回归）。NVIDIA / AMD 机器上才走 NVML / ADL。

pub mod adl;
pub mod nvml;

use std::cell::RefCell;
use std::collections::HashMap;

use crate::collector::{self, AdapterInfo, FanReading};

// ---------------------------------------------------------------- 基础类型

/// 显卡厂商。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Vendor {
    Intel,
    Nvidia,
    Amd,
    Other(u32),
}

#[allow(dead_code)] // name() 供 Phase 4/5 的 UI 来源标签使用
impl Vendor {
    pub fn from_id(id: u32) -> Self {
        match id {
            0x10DE => Vendor::Nvidia,
            0x1002 | 0x1022 => Vendor::Amd,
            0x8086 => Vendor::Intel,
            other => Vendor::Other(other),
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Vendor::Intel => "Intel",
            Vendor::Nvidia => "NVIDIA",
            Vendor::Amd => "AMD",
            Vendor::Other(_) => "其它",
        }
    }
}

/// PCI 总线/设备/功能号（BDF）。跨厂商库对卡的主键。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PciBdf {
    pub bus: u8,
    pub device: u8,
    pub function: u8,
}

#[allow(dead_code)] // format() 供 Phase 4/5 日志/UI 展示使用
impl PciBdf {
    pub fn format(&self) -> String {
        format!("{:02X}:{:02X}.{:X}", self.bus, self.device, self.function)
    }
}

/// 一张显卡的跨 API 稳定身份。
///
/// `luid` 来自 DXGI，PDH 实例名与厂商库对卡都间接依赖它；`pci_bdf` 由厂商库
/// （NVML / ADL）补充，用于库内取句柄。两者任一非零即可作为合并主键。
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct AdapterKey {
    pub vendor: Vendor,
    pub pci_bdf: PciBdf,
    pub luid: (u32, u32),
    /// DXGI 枚举序号，取专用显存规格（vram 分母）要用。
    pub index: u32,
    pub name: String,
    pub software: bool,
    pub vram_bytes: u64,
}

impl AdapterKey {
    /// PDH 实例名里的 LUID 片段（小写、各 8 位、先 High 后 Low）。
    pub fn luid_tag(&self) -> String {
        format!("luid_0x{:08x}_0x{:08x}", self.luid.0, self.luid.1)
    }
}

/// 一次 GPU 采样。所有字段独立 optional——拿不到就是 `None`，绝不补 0。
#[derive(Clone, Copy, Debug, Default)]
pub struct GpuSample {
    pub temp_c: Option<i32>,
    pub util_pct: Option<i32>,
    pub fan: Option<FanReading>,
    pub mem_used_mb: Option<u32>,
    pub mem_total_mb: Option<u32>,
    pub power_w: Option<f32>,
    pub source: Source,
}

impl GpuSample {
    /// 显存占用率（%，0..100）。
    pub fn mem_pct(&self) -> Option<i32> {
        match (self.mem_used_mb, self.mem_total_mb) {
            (Some(u), Some(t)) if t > 0 => {
                Some(((u as f64) * 100.0 / (t as f64)).round().clamp(0.0, 100.0) as i32)
            }
            _ => None,
        }
    }
}

/// 数据来源（UI "数据来源" 标签用）。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[allow(dead_code)] // Lhm/Pdh/Igl/Mock 等变体在 Phase 4/5 才由 UI 来源标签消费
pub enum Source {
    #[default]
    LhmPdh,
    Nvml,
    Adl,
    Lhm,
    Pdh,
    Igl,
    Mock,
}

impl Source {
    /// UI 友好名。
    pub fn label(&self) -> &'static str {
        match self {
            Source::LhmPdh => "LHM+PDH",
            Source::Nvml => "NVML",
            Source::Adl => "ADL",
            Source::Lhm => "LHM",
            Source::Pdh => "PDH",
            Source::Igl => "IGCL",
            Source::Mock => "Mock",
        }
    }
}

/// 后端能力位（合并时按字段取第一个非空，优先级见 `priority`）。
#[derive(Clone, Copy, Debug, Default)]
#[allow(dead_code)] // Caps 在 Phase 4/5 按能力选字段消费；当前合并走优先级
pub struct Caps {
    pub temp: bool,
    pub util: bool,
    pub fan: bool,
    pub mem: bool,
    pub power: bool,
}

// ---------------------------------------------------------------- 后端 trait

#[allow(dead_code)] // name()/caps() 供 Phase 4/5（UI 来源标签、能力选字段）使用
pub trait GpuBackend {
    fn name(&self) -> &'static str;
    fn caps(&self) -> Caps;
    fn enumerate(&self) -> Vec<AdapterKey>;
    fn read(&self, key: &AdapterKey) -> Option<GpuSample>;
    /// 诊断：PDH 命中实例数 / 总数。仅 LHM/PDH 后端有此信息，其余默认 `None`。
    fn lhm_stats(&self, _key: &AdapterKey) -> Option<(u32, u32)> {
        None
    }
}

/// 合并优先级：数值越小越优先（厂商库 > LHM/PDH 兜底）。
fn priority(src: Source) -> u8 {
    match src {
        Source::Nvml => 0,
        Source::Adl => 1,
        Source::LhmPdh => 2,
        Source::Lhm => 3,
        Source::Pdh => 4,
        Source::Igl => 5,
        // 测试用 Mock 视作"厂商库"顶优先级，确保它能覆盖 LHM/PDH（与 NVML/ADL 同档）。
        Source::Mock => 0,
    }
}

// ---------------------------------------------------------------- Mock 后端（测试用）

/// 测试用后端：返回固定样本，不碰任何系统 API。用于验证 `Resolver` 的字段合并逻辑。
#[derive(Clone)]
#[allow(dead_code)] // 仅测试构造；非测试构建会报 never constructed
pub struct MockBackend {
    pub keys: Vec<AdapterKey>,
    pub sample: GpuSample,
}

impl GpuBackend for MockBackend {
    fn name(&self) -> &'static str {
        "Mock"
    }
    fn caps(&self) -> Caps {
        Caps { temp: true, util: true, fan: true, mem: true, power: true }
    }
    fn enumerate(&self) -> Vec<AdapterKey> {
        self.keys.clone()
    }
    fn read(&self, _key: &AdapterKey) -> Option<GpuSample> {
        Some(self.sample)
    }
}

// ---------------------------------------------------------------- LHM + PDH 后端

/// 永远在场的后端：LHM 解析（温度/风扇）+ PDH 计数（利用率/显存）。
/// 对应改动前的 Intel 路径，行为完全一致（零回归）。
pub struct LhmPdhBackend {
    /// 每张卡的 PDH 利用率查询句柄（跨帧持久，差分用）。key = luid_tag。
    /// 值为 `Option` 是因为 `new_for_luid` 可能失败（PDH 打开失败）。
    states: RefCell<HashMap<String, Option<collector::PdhGpuState>>>,
}

impl LhmPdhBackend {
    pub fn new() -> Self {
        Self { states: RefCell::new(HashMap::new()) }
    }

    /// 诊断：某卡 PDH 命中实例数 / 总数（probe 打印用）。
    pub fn stats(&self, key: &AdapterKey) -> (u32, u32) {
        let tag = key.luid_tag();
        let map = self.states.borrow();
        map.get(&tag)
            .and_then(|s| s.as_ref())
            .map(|st| st.last_stats())
            .unwrap_or((0, 0))
    }
}

impl GpuBackend for LhmPdhBackend {
    fn name(&self) -> &'static str {
        "LhmPdh"
    }
    fn caps(&self) -> Caps {
        // 显存由 PDH 提供；温度/风扇来自 LHM；利用率来自 PDH。
        Caps { temp: true, util: true, fan: true, mem: true, power: false }
    }
    /// 提供 PDH 命中统计（仅本后端有此诊断信息）。
    fn lhm_stats(&self, key: &AdapterKey) -> Option<(u32, u32)> {
        Some(self.stats(key))
    }
    fn enumerate(&self) -> Vec<AdapterKey> {
        collector::enumerate_adapters()
            .into_iter()
            .map(|a: AdapterInfo| AdapterKey {
                vendor: Vendor::from_id(a.vendor_id),
                pci_bdf: PciBdf { bus: 0, device: 0, function: 0 }, // DXGI 不给 BDF；厂商库对卡用 LUID
                luid: (a.luid_high, a.luid_low),
                index: a.index,
                name: a.name.clone(),
                software: a.software,
                vram_bytes: a.dedicated_vram,
            })
            .collect()
    }
    fn read(&self, key: &AdapterKey) -> Option<GpuSample> {
        let tag = key.luid_tag();
        let mut map = self.states.borrow_mut();
        let st = map
            .entry(tag)
            .or_insert_with(|| {
                collector::PdhGpuState::new_for_luid(key.luid.0, key.luid.1)
                    .map(|s| s.with_index(key.index))
            });
        let st = st.as_mut()?;
        let util = st.sample();
        let mem = st.mem_bytes().map(|(u, t)| ((u / 1024 / 1024) as u32, (t / 1024 / 1024) as u32));
        drop(map);

        // LHM 取 GPU 温度/风扇：按选中卡的名字匹配（Phase 4 选卡修正）。
        let (_ct, _cf, gt, gf) = collector::lhm_sample(Some(&key.name));

        let mut s = GpuSample {
            temp_c: gt,
            util_pct: util,
            fan: gf,
            source: Source::LhmPdh,
            ..Default::default()
        };
        if let Some((used, total)) = mem {
            if total > 0 {
                s.mem_used_mb = Some(used);
                s.mem_total_mb = Some(total);
            }
        }
        Some(s)
    }
}

// ---------------------------------------------------------------- Resolver（多后端合并）

/// 把多个 `GpuBackend` 合并成一个统一视图。
///
/// - `enumerate()` 合并去重（按 luid 或 BDF）；
/// - `read(key)` 对每个字段取**优先级最高**的非空后端（厂商库 > LHM/PDH）；
/// - 选卡默认"显存最大的非软件独显"，`force()` 允许 UI 覆盖。
pub struct Resolver {
    backends: Vec<Box<dyn GpuBackend>>,
    forced: Option<AdapterKey>,
}

#[allow(dead_code)] // force / read_primary / with_backends 供 Phase 4/5（UI 来源标签、选卡覆盖）使用
impl Resolver {
    /// 构造并自动注册所有可用后端。NVML / ADL 在 DLL 不存在时自动跳过。
    pub fn new() -> Self {
        let mut backends: Vec<Box<dyn GpuBackend>> = Vec::new();
        backends.push(Box::new(LhmPdhBackend::new()));
        if let Some(b) = nvml::NvmlBackend::try_new() {
            backends.push(Box::new(b));
        }
        if let Some(b) = adl::AdlBackend::try_new() {
            backends.push(Box::new(b));
        }
        Self { backends, forced: None }
    }

    /// 仅供测试：用指定的后端列表构造。
    pub fn with_backends(backends: Vec<Box<dyn GpuBackend>>) -> Self {
        Self { backends, forced: None }
    }

    /// UI 覆盖选卡。
    pub fn force(&mut self, key: AdapterKey) {
        self.forced = Some(key);
    }

    pub fn enumerate(&self) -> Vec<AdapterKey> {
        let mut out: Vec<AdapterKey> = Vec::new();
        for b in &self.backends {
            for k in b.enumerate() {
                if !out.iter().any(|e| key_eq(e, &k)) {
                    out.push(k);
                }
            }
        }
        out
    }

    /// 默认主卡：显存最大的非软件适配器；`force()` 优先。
    pub fn select_primary(&self) -> Option<AdapterKey> {
        if let Some(k) = &self.forced {
            return Some(k.clone());
        }
        self.enumerate()
            .into_iter()
            .filter(|k| !k.software)
            .max_by_key(|k| k.vram_bytes)
    }

    /// 读某张卡：合并所有后端、按优先级填字段。
    pub fn read(&self, key: &AdapterKey) -> Option<GpuSample> {
        let mut collected: Vec<(u8, GpuSample)> = Vec::new();
        for b in &self.backends {
            if let Some(s) = b.read(key) {
                collected.push((priority(s.source), s));
            }
        }
        if collected.is_empty() {
            return None;
        }
        // 优先级数字越小越高（Nvml=0 最高）。按升序排列，使高优先级后端先填字段，
        // 配合下方 `out.x.or(s.x)` 的语义 —— 高优先级后端的值先锁定，低优先级只补它
        // 没提供的字段，体现"厂商库覆盖 LHM/PDH"的合并策略。
        collected.sort_by_key(|(p, _)| *p);
        let mut out = GpuSample::default();
        for (_, s) in &collected {
            out.temp_c = out.temp_c.or(s.temp_c);
            out.util_pct = out.util_pct.or(s.util_pct);
            out.fan = out.fan.or(s.fan);
            out.mem_used_mb = out.mem_used_mb.or(s.mem_used_mb);
            out.mem_total_mb = out.mem_total_mb.or(s.mem_total_mb);
            out.power_w = out.power_w.or(s.power_w);
        }
        out.source = collected[0].1.source; // 最高优先级后端的来源标签
        Some(out)
    }

    /// 读主卡（选卡优先）。
    pub fn read_primary(&self) -> Option<GpuSample> {
        self.select_primary().and_then(|k| self.read(&k))
    }

    /// 诊断：LHM/PDH 后端的 PDH 命中统计。
    pub fn lhm_pdh_stats(&self, key: &AdapterKey) -> (u32, u32) {
        for b in &self.backends {
            if let Some((m, t)) = b.lhm_stats(key) {
                return (m, t);
            }
        }
        (0, 0)
    }
}

/// 合并去重：luid 相同、或 BDF 相同、或名字归一化后相同，即视为同一张卡。
///
/// 厂商库（NVML/ADL）报 BDF 但不报 LUID；LHM/PDH 报 LUID 但不报 BDF，
/// 两者靠名字归一匹配合并成同一张卡。名字相同误合并极罕见（同型号双卡），
/// Phase 4 再做更严谨的映射。
fn key_eq(a: &AdapterKey, b: &AdapterKey) -> bool {
    if a.luid != (0, 0) && a.luid == b.luid {
        return true;
    }
    if a.pci_bdf.bus != 0 && a.pci_bdf == b.pci_bdf {
        return true;
    }
    let na = a.name.trim().to_ascii_lowercase();
    let nb = b.name.trim().to_ascii_lowercase();
    !na.is_empty() && na == nb
}

// ---------------------------------------------------------------- GpuUsage 兼容门面

/// 对外兼容门面：行为与原 `collector::GpuUsage` 完全一致（app.rs / main.rs 不改调用）。
///
/// 内部持有 `Resolver` + 选定的主卡 `AdapterKey`，所有方法委托给 Resolver。
/// 本机只有 `LhmPdhBackend` 注册时，数值与改动前逐位相同。
pub struct GpuUsage {
    resolver: Resolver,
    key: AdapterKey,
}

impl GpuUsage {
    /// 自动选主 GPU（显存最大的非软件适配器）。
    pub fn new() -> Option<Self> {
        let r = Resolver::new();
        let key = r.select_primary()?;
        Some(Self { resolver: r, key })
    }

    /// 指定某张卡（按 LUID 高/低 32 位）。
    pub fn new_for_luid(luid_high: u32, luid_low: u32) -> Option<Self> {
        let r = Resolver::new();
        let key = r.enumerate().into_iter().find(|k| k.luid == (luid_high, luid_low))?;
        Some(Self { resolver: r, key })
    }

    /// 绑定 DXGI 序号（取显存分母用）。兼容原 `with_index`。
    pub fn with_index(mut self, index: u32) -> Self {
        self.key.index = index;
        self
    }

    /// 这张卡的名字。
    pub fn adapter_name(&self) -> Option<&str> {
        Some(&self.key.name)
    }

    /// 取一次完整采样（每帧调一次即可，内部只驱动一次 PDH 差分）。
    pub fn read_full(&self) -> Option<GpuSample> {
        self.resolver.read(&self.key)
    }

    /// GPU 核心利用率（%）。兼容原 `sample()`。
    pub fn sample(&mut self) -> Option<i32> {
        self.resolver.read(&self.key).and_then(|s| s.util_pct)
    }

    /// 显存占用率（%）。兼容原 `vram_percent()`。
    pub fn vram_percent(&mut self) -> Option<i32> {
        self.resolver.read(&self.key).and_then(|s| s.mem_pct())
    }

    /// (命中实例数, 全部实例数)。诊断用。
    pub fn last_stats(&self) -> (u32, u32) {
        self.resolver.lhm_pdh_stats(&self.key)
    }

    /// 列出所有已知适配器（多后端合并去重）。供 UI 选卡下拉（Phase 4）。
    pub fn enumerate(&self) -> Vec<AdapterKey> {
        self.resolver.enumerate()
    }

    /// 读指定适配器（不依赖当前选中的主卡）。供 Phase 5 取副卡数据。
    pub fn read_key(&self, key: &AdapterKey) -> Option<GpuSample> {
        self.resolver.read(key)
    }

    /// UI 选卡覆盖：下拉选哪张，采集就走哪张（Phase 4）。
    pub fn force(&mut self, key: AdapterKey) {
        self.resolver.force(key);
    }

    /// 仅供测试：用一个现成的 `Resolver` 构造门面（注入虚拟/录制后端）。
    #[cfg(test)]
    pub fn from_resolver(resolver: Resolver) -> Option<Self> {
        let key = resolver.select_primary()?;
        Some(Self { resolver, key })
    }
}

/// 构造一个 DXGI 主卡的 `AdapterKey`（测试/调试用）。
#[allow(dead_code)]
pub fn primary_key() -> Option<AdapterKey> {
    Resolver::new().select_primary()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 验证 Resolver 字段合并：低优先级的 LHM/PDH 填了 mem，高优先级的 Mock 填了
    /// temp/util/fan/power，合并后两者都应存在。
    #[test]
    fn resolver_merges_fields_by_priority() {
        let lhm = LhmPdhBackend::new();
        // 用真实 DXGI 主卡当 key（本机有 A770 就能拿到；没有则跳过整个测试）
        let Some(key) = lhm.enumerate().into_iter().find(|k| !k.software) else {
            return;
        };

        let mock = MockBackend {
            keys: vec![key.clone()],
            sample: GpuSample {
                temp_c: Some(62),
                util_pct: Some(34),
                fan: Some(FanReading::percent(50.0)),
                power_w: Some(120.5),
                source: Source::Mock,
                ..Default::default()
            },
        };

        let r = Resolver::with_backends(vec![Box::new(lhm), Box::new(mock)]);
        let s = r.read(&key).expect("应能读到");

        // Mock 高优先级：temp/util/fan/power 来自它
        assert_eq!(s.temp_c, Some(62));
        assert_eq!(s.util_pct, Some(34));
        assert_eq!(s.fan.map(|f| f.display()), Some(50));
        assert_eq!(s.power_w, Some(120.5));
        // LHM/PDH 提供的 mem 应被保留（Mock 没给 mem）
        assert!(s.mem_used_mb.is_some() && s.mem_total_mb.is_some());
    }

    /// 验证：`LhmPdhBackend` 单后端时，读到的 sample 来源标 `LhmPdh`。
    #[test]
    fn lhm_pdh_source_tagged() {
        let b = LhmPdhBackend::new();
        let Some(key) = b.enumerate().into_iter().find(|k| !k.software) else {
            return;
        };
        let s = b.read(&key).expect("应能读到");
        assert_eq!(s.source, Source::LhmPdh);
    }

    /// 厂商库（Mock 模拟 NVML）应覆盖 LHM 的 temp——即便 LHM 也回报 temp。
    #[test]
    fn vendor_backend_overrides_lhm_temp() {
        let lhm = LhmPdhBackend::new();
        let Some(key) = lhm.enumerate().into_iter().find(|k| !k.software) else {
            return;
        };
        // 制造一个"LHM 给 99°C、NVML 给 62°C"的冲突，看合并结果
        let lhm_with_temp = MockBackend {
            keys: vec![key.clone()],
            sample: GpuSample { temp_c: Some(99), source: Source::Lhm, ..Default::default() },
        };
        let nvml = MockBackend {
            keys: vec![key.clone()],
            sample: GpuSample { temp_c: Some(62), source: Source::Nvml, ..Default::default() },
        };
        // 顺序：先放 lhm，再放 nvml（NVML 优先级高）
        let r = Resolver::with_backends(vec![Box::new(lhm_with_temp), Box::new(nvml)]);
        let s = r.read(&key).expect("应能读到");
        assert_eq!(s.temp_c, Some(62), "NVML 应覆盖 LHM 的温度");
    }

    /// 空后端 / 未知 key 应返回 None，不 panic。
    #[test]
    fn no_backend_returns_none() {
        let r = Resolver::with_backends(vec![]);
        assert!(r.read_primary().is_none());
    }

    // ---------------------------------------------------------------- 虚拟后端（网络样本回放）

    use crate::collector::CpuUsage;
    use serde_json::Value;

    /// 测试用"录制后端"：把从网上提取的物理样本（fixtures/web_samples.json）按来源拆成
    /// 多个后端，模拟 NVML / ADL / LHM+PDH 在真实多卡机器上的行为。它**不碰任何系统
    /// API**，只回放样本 —— 用于验证"识别设备 + 输出数据"的逻辑链路（与真机 DLL 调用
    /// 等价，仅把最底层的 FFI 取数替换为回放）。
    struct RecordedBackend {
        source: Source,
        cards: Vec<(AdapterKey, GpuSample)>,
    }

    impl GpuBackend for RecordedBackend {
        fn name(&self) -> &'static str {
            match self.source {
                Source::Nvml => "NVML(replay)",
                Source::Adl => "ADL(replay)",
                Source::LhmPdh => "LHM+PDH(replay)",
                other => other.label(),
            }
        }
        fn caps(&self) -> Caps {
            Caps { temp: true, util: true, fan: true, mem: true, power: true }
        }
        fn enumerate(&self) -> Vec<AdapterKey> {
            self.cards.iter().map(|(k, _)| k.clone()).collect()
        }
        fn read(&self, key: &AdapterKey) -> Option<GpuSample> {
            self.cards.iter().find(|(k, _)| key_eq(k, key)).map(|(_, s)| *s)
        }
    }

    fn bdf_from_str(s: &str) -> PciBdf {
        let mut parts = s.split(':');
        let bus = u8::from_str_radix(parts.next().unwrap_or("0"), 16).unwrap_or(0);
        let mut devfn = parts.next().unwrap_or("0").split('.');
        let device = u8::from_str_radix(devfn.next().unwrap_or("0"), 16).unwrap_or(0);
        let function = u8::from_str_radix(devfn.next().unwrap_or("0"), 16).unwrap_or(0);
        PciBdf { bus, device, function }
    }

    fn key_from_json(c: &Value) -> AdapterKey {
        let luid = c["luid"].as_array().expect("luid 数组");
        let vendor = match c["vendor"].as_str() {
            Some("NVIDIA") => Vendor::from_id(0x10DE),
            Some("AMD") => Vendor::from_id(0x1002),
            Some("Intel") => Vendor::from_id(0x8086),
            _ => Vendor::Other(0),
        };
        AdapterKey {
            vendor,
            pci_bdf: bdf_from_str(c["bdf"].as_str().unwrap_or("00:00.0")),
            luid: (luid[0].as_u64().unwrap() as u32, luid[1].as_u64().unwrap() as u32),
            index: c["index"].as_u64().unwrap() as u32,
            name: c["name"].as_str().unwrap().to_string(),
            software: c["software"].as_bool().unwrap_or(false),
            vram_bytes: c["vram_mb"].as_u64().unwrap() * 1024 * 1024,
        }
    }

    fn sample_from_json(c: &Value, source: Source) -> GpuSample {
        let fan = c["fan_rpm"].as_i64().filter(|&v| v > 0).map(|v| FanReading::rpm(v as f64));
        GpuSample {
            temp_c: c["temp_c"].as_i64().map(|v| v as i32),
            util_pct: c["util_pct"].as_i64().map(|v| v as i32),
            fan,
            mem_used_mb: c["mem_used_mb"].as_u64().map(|v| v as u32),
            mem_total_mb: c["mem_total_mb"].as_u64().map(|v| v as u32),
            power_w: c["power_w"].as_f64().map(|v| v as f32),
            source,
        }
    }

    /// 虚拟测试（用户需求）：用从网上提取的 NVIDIA RTX 4070 / AMD RX 7800 XT 真实物理
    /// 样本，驱动真实代码路径（Resolver 识别 → collect 采集 → build_payload 组帧），
    /// 验证上位机能够在"N 卡 + A 卡"机器上识别设备并输出数据。
    #[test]
    fn virtual_multi_vendor_recognition_and_output() {
        let raw = include_str!("fixtures/web_samples.json");
        let doc: Value = serde_json::from_str(raw).expect("web_samples.json 应合法");
        let cards = doc["cards"].as_array().expect("cards 数组");

        // 拆成三个"录制后端"，模拟真实多卡主机：
        //   · LHM+PDH 回放：枚举全部 3 张（DXGI 视角，LUID 主键），但只对 Intel 给
        //     temp/fan/power；NVIDIA/AMD 仅给 util + mem（模拟 LHM/PDH 在真实主机上对
        //     N/A 卡缺乏温度/风扇读数，需要厂商库补）。
        //   · NVML 回放：只枚举 NVIDIA（BDF 主键 + 同名），给全量 temp/util/fan/power。
        //   · ADL  回放：只枚举 AMD  （BDF 主键 + 同名），给全量 temp/util/fan/power。
        let mut lhm_cards = Vec::new();
        let mut nvml_cards = Vec::new();
        let mut adl_cards = Vec::new();
        for c in cards {
            let key = key_from_json(c);
            let only = c["lhmpdh_only"].as_bool().unwrap_or(false);
            if only {
                // Intel：LHM+PDH 给全量
                lhm_cards.push((key.clone(), sample_from_json(c, Source::LhmPdh)));
            } else {
                // NVIDIA/AMD：LHM+PDH 只给 util + mem（精简样本）
                let slim = GpuSample {
                    util_pct: c["util_pct"].as_i64().map(|v| v as i32),
                    mem_used_mb: c["mem_used_mb"].as_u64().map(|v| v as u32),
                    mem_total_mb: c["mem_total_mb"].as_u64().map(|v| v as u32),
                    source: Source::LhmPdh,
                    ..Default::default()
                };
                lhm_cards.push((key.clone(), slim));
            }
            match c["vendor"].as_str() {
                Some("NVIDIA") => nvml_cards.push((key.clone(), sample_from_json(c, Source::Nvml))),
                Some("AMD") => adl_cards.push((key.clone(), sample_from_json(c, Source::Adl))),
                _ => {}
            }
        }

        let lhm = RecordedBackend { source: Source::LhmPdh, cards: lhm_cards };
        let nvml = RecordedBackend { source: Source::Nvml, cards: nvml_cards };
        let adl = RecordedBackend { source: Source::Adl, cards: adl_cards };

        let r = Resolver::with_backends(vec![Box::new(lhm), Box::new(nvml), Box::new(adl)]);

        // 1) 识别：枚举到 3 张不同厂商的卡
        let adapters = r.enumerate();
        println!("识别到适配器 {} 张:", adapters.len());
        for a in &adapters {
            println!(
                "  [{}] {}  vram={:.2}GiB",
                a.vendor.name(),
                a.name,
                a.vram_bytes as f64 / 1024.0 / 1024.0 / 1024.0
            );
        }
        assert_eq!(adapters.len(), 3, "应识别 3 张卡");
        assert!(adapters.iter().any(|a| a.vendor == Vendor::Intel));
        assert!(adapters.iter().any(|a| a.vendor == Vendor::Nvidia));
        assert!(adapters.iter().any(|a| a.vendor == Vendor::Amd));

        // 2) 逐卡读取，验证厂商库覆盖 + 来源标签
        for a in &adapters {
            let s = r.read(a).expect("应能读到");
            println!(
                "  读取 {}: temp={:?}C util={:?}% fan={:?} power={:?}W 来源={}",
                a.name,
                s.temp_c,
                s.util_pct,
                s.fan.map(|f| f.raw),
                s.power_w,
                s.source.label()
            );
            match a.vendor {
                Vendor::Intel => {
                    assert_eq!(s.source, Source::LhmPdh, "Intel 走 LHM+PDH");
                    assert_eq!(s.temp_c, Some(53));
                }
                Vendor::Nvidia => {
                    assert_eq!(s.source, Source::Nvml, "NVIDIA 必须被 NVML 识别并标记来源");
                    assert_eq!(s.temp_c, Some(64), "温度应来自 NVML 回放");
                    assert_eq!(s.fan.map(|f| f.raw), Some(1430), "风扇应来自 NVML 回放(RPM)");
                    assert_eq!(s.power_w, Some(200.0), "功耗应来自 NVML 回放");
                }
                Vendor::Amd => {
                    assert_eq!(s.source, Source::Adl, "AMD 必须被 ADL 识别并标记来源");
                    assert_eq!(s.temp_c, Some(70), "温度应来自 ADL 回放");
                    assert_eq!(s.fan.map(|f| f.raw), Some(1315), "风扇应来自 ADL 回放(RPM)");
                    assert_eq!(s.power_w, Some(250.0), "功耗应来自 ADL 回放");
                }
                _ => {}
            }
        }

        // 3) 走真实采集路径 collect() + 协议组帧，验证主卡功耗(GP)与副卡(T2/U2/F2)输出
        let gpu = GpuUsage::from_resolver(r).expect("应能构造门面");
        let mut cpu = CpuUsage::new();
        let sample = crate::collector::collect(&mut cpu, &mut Some(gpu));

        // 主卡 = 显存最大 = AMD(16GiB) → GP=250, GT=70, GF=1315
        assert_eq!(sample.gp, Some(250), "主卡功耗(GP)应来自 AMD");
        assert_eq!(sample.gt, Some(70), "主卡温度应来自 AMD");
        assert_eq!(sample.gf, Some(1315), "主卡风扇(RPM)应来自 AMD");
        // 副卡 = 显存第二大 = NVIDIA(12GiB) → T2=64, U2=98, F2=1430
        assert_eq!(sample.gt2, Some(64), "副卡温度(T2)应来自 NVIDIA");
        assert_eq!(sample.gu2, Some(98), "副卡利用率(U2)应来自 NVIDIA");
        assert_eq!(sample.gf2, Some(1430), "副卡风扇(F2)应来自 NVIDIA");
        assert_eq!(sample.source, Source::Adl, "主卡来源应为 ADL");

        // 4) 组帧验证：把 Sample 拼成固件协议帧，确认新 key 真的出现在链路上
        let mask: &[(bool, &str)] = &[
            (true, "CT"), (true, "CU"), (true, "CF"),
            (true, "GT"), (true, "GU"), (true, "GF"),
            (true, "GP"), (true, "T2"), (true, "U2"), (true, "F2"),
            (true, "GV"), (true, "MU"),
        ];
        let payload = crate::board::build_payload(&sample, mask);
        println!("组帧 payload: {}", payload);
        assert!(payload.contains("GP:250"), "帧里应有主卡功耗 GP:250");
        assert!(payload.contains("T2:64"), "帧里应有副卡温度 T2:64");
        assert!(payload.contains("U2:98"), "帧里应有副卡利用率 U2:98");
        assert!(payload.contains("F2:1430"), "帧里应有副卡风扇 F2:1430");
        assert!(payload.contains("GT:70"), "帧里应有主卡温度 GT:70");
        assert!(payload.contains("GF:1315"), "帧里应有主卡风扇 GF:1315");
    }
}
