//! AMD ADL 后端（Phase 3，对应 `docs/上位机GPU取数规划_AMD_NVIDIA.md` §5 Phase 3）。
//!
//! 用 `libloading` 动态加载 `atiadlxx.dll`，加载失败即视为"本机无 AMD"（不注册）。
//! ADL 是 C API，`ADL_Main_Control_Create` 需要传一个回调（标准样板）。
//!
//! ⚠ 本机（Intel Arc A770）没有 `atiadlxx.dll`，`try_new()` 返回 None，不注册 —— 零回归。
//! 代码路径仅真有 A 卡的机器上生效，靠 Mock 与真机探针验证（见 §6）。
//!
//! 风险：ADL 结构体布局随驱动版本略有差异，真机首次接入需用 `--probe-gpu` 校准
//! （见文档 §6 探针用法）。OD6 失效时（新 RDNA）可降级到 LHM/PDH。

use std::collections::HashMap;
use std::os::raw::c_void;

use libloading::{Library, Symbol};
use crate::collector::FanReading;
use super::{AdapterKey, Caps, GpuBackend, GpuSample, PciBdf, Source, Vendor};

type AdlMainMenuCreate = unsafe extern "C" fn(i32, *mut c_void) -> i32;
type AdlMainControlCreate = unsafe extern "C" fn(AdlMainMenuCreate, i32) -> i32;

pub struct AdlBackend {
    lib: Library,
    /// BDF → ADL 适配器序号（enumerate 时建立，read 时用）。
    index_of: std::cell::RefCell<HashMap<PciBdf, i32>>,
}

impl AdlBackend {
    pub fn try_new() -> Option<Self> {
        let lib = unsafe { Library::new("atiadlxx.dll") }.ok()?;
        let create: Result<Symbol<AdlMainControlCreate>, _> =
            unsafe { lib.get(b"ADL_Main_Control_Create\0") };
        let create = create.ok()?;
        // 标准 ADL 样板：回调返回 1 表示成功（ADL 用回调做内存分配）。
        extern "C" fn cb(_enum: i32, _ctx: *mut c_void) -> i32 {
            1
        }
        if unsafe { create(cb, 1) } != 0 {
            return None;
        }
        let b = Self { lib, index_of: Default::default() };
        b.build_index();
        Some(b)
    }

    fn build_index(&self) {
        let num: Symbol<unsafe extern "C" fn(*mut i32) -> i32> =
            match unsafe { self.lib.get(b"ADL_Adapter_NumberOfAdapters_Get\0") } {
                Ok(s) => s,
                Err(_) => return,
            };
        let mut n: i32 = 0;
        if unsafe { num(&mut n) } != 0 || n <= 0 {
            return;
        }
        let pci: Symbol<unsafe extern "C" fn(i32, *mut i32, *mut i32, *mut i32) -> i32> =
            match unsafe { self.lib.get(b"ADL_Adapter_PciBusNumber\0") } {
                Ok(s) => s,
                Err(_) => return,
            };
        let mut map = self.index_of.borrow_mut();
        for i in 0..n {
            let mut bus: i32 = 0;
            let mut dev: i32 = 0;
            let mut func: i32 = 0;
            if unsafe { pci(i, &mut bus, &mut dev, &mut func) } == 0 {
                map.insert(
                    PciBdf { bus: bus as u8, device: dev as u8, function: func as u8 },
                    i,
                );
            }
        }
    }

    fn sym<T>(&self, name: &[u8]) -> Option<Symbol<'_, T>> {
        unsafe { self.lib.get(name).ok() }
    }
}

impl Drop for AdlBackend {
    fn drop(&mut self) {
        if let Ok(d) =
            unsafe { self.lib.get::<unsafe extern "C" fn() -> i32>(b"ADL_Main_Control_Destroy\0") }
        {
            unsafe { d(); }
        }
    }
}

impl GpuBackend for AdlBackend {
    fn name(&self) -> &'static str {
        "Adl"
    }
    fn caps(&self) -> Caps {
        // 温度/风扇/利用率走 ADL；显存走 PDH（文档 §4）；功耗接口不稳，先不取。
        Caps { temp: true, util: true, fan: true, mem: false, power: false }
    }
    fn enumerate(&self) -> Vec<AdapterKey> {
        let map = self.index_of.borrow();
        map.iter()
            .map(|(bdf, &idx)| AdapterKey {
                vendor: Vendor::Amd,
                pci_bdf: *bdf,
                luid: (0, 0),
                index: idx as u32,
                name: format!("AMD Radeon #{}", idx),
                software: false,
                vram_bytes: 0,
            })
            .collect()
    }
    fn read(&self, key: &AdapterKey) -> Option<GpuSample> {
        let idx = *self.index_of.borrow().get(&key.pci_bdf)?;
        let mut s = GpuSample {
            source: Source::Adl,
            ..Default::default()
        };
        // 温度
        if let Some(t) = self.sym::<unsafe extern "C" fn(i32, *mut i32) -> i32>(
            b"ADL_Overdrive6_Temperature_Get\0",
        ) {
            let mut v: i32 = 0;
            if unsafe { t(idx, &mut v) } == 0 {
                s.temp_c = Some(v);
            }
        }
        // 活动（GPU 利用率）
        if let Some(a) = self.sym::<unsafe extern "C" fn(i32, *mut AdlCurrentActivity) -> i32>(
            b"ADL_Overdrive6_CurrentActivity_Get\0",
        ) {
            let mut act = AdlCurrentActivity::default();
            if unsafe { a(idx, &mut act) } == 0 {
                s.util_pct = Some(act.i_activity_percent);
            }
        }
        // 风扇
        if let Some(f) = self.sym::<
            unsafe extern "C" fn(i32, *mut AdlOd6FanSpeedInfo, *mut i32) -> i32,
        >(b"ADL_Overdrive6_FanSpeed_Get\0")
        {
            let mut info = AdlOd6FanSpeedInfo::default();
            let mut pct: i32 = 0;
            if unsafe { f(idx, &mut info, &mut pct) } == 0 {
                // iFanSpeedType: 1 = 百分比，0 = RPM
                if info.i_fan_speed_type == 1 {
                    s.fan = Some(FanReading::percent(pct as f64));
                } else {
                    s.fan = Some(FanReading::rpm(info.i_fan_speed as f64));
                }
            }
        }
        Some(s)
    }
}

#[repr(C)]
#[derive(Default)]
struct AdlCurrentActivity {
    i_size: i32,
    i_engine_clock: i32,
    i_memory_clock: i32,
    i_vddc: i32,
    i_activity_percent: i32,
    i_current_performance_level: i32,
    i_current_bus_speed: i32,
    i_current_bus_lanes: i32,
    i_maximum_bus_lanes: i32,
    i_reserved: [i32; 16],
}

#[repr(C)]
#[derive(Default)]
struct AdlOd6FanSpeedInfo {
    i_size: i32,
    i_fan_speed: i32,
    i_fan_speed_rpm: i32,
    i_fan_speed_type: i32,
    i_min_fan_limit: i32,
    i_max_fan_limit: i32,
    i_fan_control_mode: i32,
    i_fan_control_value: i32,
    i_manual_fan_control_value: i32,
    i_min_fan_rpm: i32,
    i_max_fan_rpm: i32,
}
