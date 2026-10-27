//! NVIDIA NVML 后端（Phase 2，对应 `docs/上位机GPU取数规划_AMD_NVIDIA.md` §5 Phase 2）。
//!
//! 用 `libloading` 动态加载 `nvml.dll`，加载失败即视为"本机无 NVIDIA"（不注册）。
//! 所有采集在调用线程完成（采集线程），不碰 UI 线程。
//!
//! ⚠ 本机（Intel Arc A770）没有 `nvml.dll`，`try_new()` 返回 `None`，
//! 因此本后端**不会被 `Resolver` 注册** —— 行为与改动前完全一致（零回归）。
//! 代码路径仅在真有 N 卡的机器上生效，靠 Mock 与真机探针验证（见 §6）。

use std::ffi::{CStr, CString};

use libloading::{Library, Symbol};
use crate::collector::FanReading;
use super::{AdapterKey, Caps, GpuBackend, GpuSample, PciBdf, Source, Vendor};

type NvmlDevice = *mut std::os::raw::c_void;

#[repr(C)]
struct NvmlUtilization {
    gpu: u32,
    memory: u32,
    encoder: u32,
    decoder: u32,
}

const NVML_SUCCESS: i32 = 0;
const NVML_TEMPERATURE_GPU: u32 = 0;

pub struct NvmlBackend {
    lib: Library,
}

impl NvmlBackend {
    /// 加载 nvml.dll 并完成初始化；失败返回 None（本机无 N 卡）。
    pub fn try_new() -> Option<Self> {
        let lib = unsafe { Library::new("nvml.dll") }.ok()?;
        // 验证关键符号存在，否则放弃
        let _init: Result<Symbol<unsafe extern "C" fn() -> i32>, _> =
            unsafe { lib.get(b"nvmlInit_v2\0") };
        if _init.is_err() {
            return None;
        }
        let b = Self { lib };
        let init: Symbol<unsafe extern "C" fn() -> i32> =
            unsafe { b.lib.get(b"nvmlInit_v2\0") }.ok()?;
        if unsafe { init() } != NVML_SUCCESS {
            return None;
        }
        Some(b)
    }

    fn sym<T>(&self, name: &[u8]) -> Option<Symbol<'_, T>> {
        unsafe { self.lib.get(name).ok() }
    }

    /// 按 BDF 取设备句柄。
    fn handle_by_bdf(&self, bdf: PciBdf) -> Option<NvmlDevice> {
        let bus_id =
            CString::new(format!("00000000:{:02X}:{:02X}.{}", bdf.bus, bdf.device, bdf.function))
                .ok()?;
        let get: Symbol<unsafe extern "C" fn(*const std::os::raw::c_char, *mut NvmlDevice) -> i32> =
            self.sym(b"nvmlDeviceGetHandleByPciBusId_v2\0")?;
        let mut handle: NvmlDevice = std::ptr::null_mut();
        if unsafe { get(bus_id.as_ptr(), &mut handle) } != NVML_SUCCESS || handle.is_null() {
            return None;
        }
        Some(handle)
    }
}

impl Drop for NvmlBackend {
    fn drop(&mut self) {
        if let Ok(shutdown) =
            unsafe { self.lib.get::<unsafe extern "C" fn() -> i32>(b"nvmlShutdown\0") }
        {
            unsafe { shutdown(); }
        }
    }
}

impl GpuBackend for NvmlBackend {
    fn name(&self) -> &'static str {
        "Nvml"
    }
    fn caps(&self) -> Caps {
        // 显存走 PDH（见文档 §4），其余 NVML 提供。
        Caps { temp: true, util: true, fan: true, mem: false, power: true }
    }
    fn enumerate(&self) -> Vec<AdapterKey> {
        let count: Symbol<unsafe extern "C" fn(*mut u32) -> i32> =
            match self.sym(b"nvmlDeviceGetCount_v2\0") {
                Some(s) => s,
                None => return vec![],
            };
        let mut n: u32 = 0;
        if unsafe { count(&mut n) } != NVML_SUCCESS {
            return vec![];
        }
        let mut out = Vec::new();
        for i in 0..n {
            let idx: Symbol<unsafe extern "C" fn(u32, *mut NvmlDevice) -> i32> =
                match self.sym(b"nvmlDeviceGetHandleByIndex_v2\0") {
                    Some(s) => s,
                    None => break,
                };
            let mut h: NvmlDevice = std::ptr::null_mut();
            if unsafe { idx(i, &mut h) } != NVML_SUCCESS || h.is_null() {
                continue;
            }
            // BDF
            let mut busbuf = [0 as std::os::raw::c_char; 32];
            let gbid: Symbol<
                unsafe extern "C" fn(NvmlDevice, *mut std::os::raw::c_char, u32) -> i32,
            > = match self.sym(b"nvmlDeviceGetPciBusId_v3\0") {
                Some(s) => s,
                None => break,
            };
            if unsafe { gbid(h, busbuf.as_mut_ptr(), 32) } != NVML_SUCCESS {
                continue;
            }
            let busstr = unsafe { CStr::from_ptr(busbuf.as_ptr()) }
                .to_string_lossy()
                .to_string();
            let bdf = parse_bdf(&busstr).unwrap_or(PciBdf { bus: 0, device: 0, function: 0 });
            // 名字
            let mut namebuf = [0 as std::os::raw::c_char; 256];
            let gname: Symbol<
                unsafe extern "C" fn(NvmlDevice, *mut std::os::raw::c_char, u32) -> i32,
            > = match self.sym(b"nvmlDeviceGetName_v2\0") {
                Some(s) => s,
                None => break,
            };
            let name = if unsafe { gname(h, namebuf.as_mut_ptr(), 256) } == NVML_SUCCESS {
                unsafe { CStr::from_ptr(namebuf.as_ptr()) }
                    .to_string_lossy()
                    .trim()
                    .to_string()
            } else {
                format!("NVIDIA GPU #{}", i)
            };
            out.push(AdapterKey {
                vendor: Vendor::Nvidia,
                pci_bdf: bdf,
                luid: (0, 0),
                index: i,
                name,
                software: false,
                vram_bytes: 0,
            });
        }
        out
    }
    fn read(&self, key: &AdapterKey) -> Option<GpuSample> {
        let handle = self.handle_by_bdf(key.pci_bdf)?;
        let mut s = GpuSample {
            source: Source::Nvml,
            ..Default::default()
        };
        // 温度
        if let Some(t) = self.sym::<unsafe extern "C" fn(NvmlDevice, u32, *mut u32) -> i32>(
            b"nvmlDeviceGetTemperature\0",
        ) {
            let mut v: u32 = 0;
            if unsafe { t(handle, NVML_TEMPERATURE_GPU, &mut v) } == NVML_SUCCESS {
                s.temp_c = Some(v as i32);
            }
        }
        // 利用率
        if let Some(u) = self.sym::<unsafe extern "C" fn(NvmlDevice, *mut NvmlUtilization) -> i32>(
            b"nvmlDeviceGetUtilizationRates\0",
        ) {
            let mut util = NvmlUtilization { gpu: 0, memory: 0, encoder: 0, decoder: 0 };
            if unsafe { u(handle, &mut util) } == NVML_SUCCESS {
                s.util_pct = Some(util.gpu as i32);
            }
        }
        // 风扇（百分比）→ FanReading::percent
        if let Some(f) = self.sym::<unsafe extern "C" fn(NvmlDevice, *mut u32) -> i32>(
            b"nvmlDeviceGetFanSpeed_v2\0",
        ) {
            let mut v: u32 = 0;
            if unsafe { f(handle, &mut v) } == NVML_SUCCESS {
                s.fan = Some(FanReading::percent(v as f64));
            }
        }
        // 功耗（毫瓦 → 瓦）
        if let Some(p) = self.sym::<unsafe extern "C" fn(NvmlDevice, *mut u32) -> i32>(
            b"nvmlDeviceGetPowerUsage\0",
        ) {
            let mut v: u32 = 0;
            if unsafe { p(handle, &mut v) } == NVML_SUCCESS {
                s.power_w = Some((v as f32) / 1000.0);
            }
        }
        Some(s)
    }
}

/// 解析 NVML busId 字符串：`00000000:01:00.0` / `0000:0A:1F.3` / `0x0000:05:00.0`。
fn parse_bdf(s: &str) -> Option<PciBdf> {
    let s = s.trim();
    let parts: Vec<&str> = s.split(':').collect();
    if parts.len() < 2 {
        return None;
    }
    let bus = u8::from_str_radix(parts[parts.len() - 2].trim_start_matches("0x"), 16).ok()?;
    let dev_func = parts[parts.len() - 1];
    let mut it = dev_func.split('.');
    let device = u8::from_str_radix(it.next()?, 16).ok()?;
    let function = u8::from_str_radix(it.next().unwrap_or("0"), 16).ok()?;
    Some(PciBdf { bus, device, function })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parse_bdf_formats() {
        assert_eq!(
            parse_bdf("00000000:01:00.0"),
            Some(PciBdf { bus: 1, device: 0, function: 0 })
        );
        assert_eq!(
            parse_bdf("0000:0A:1F.3"),
            Some(PciBdf { bus: 10, device: 31, function: 3 })
        );
        assert_eq!(
            parse_bdf("0x0000:05:00.0"),
            Some(PciBdf { bus: 5, device: 0, function: 0 })
        );
    }
}
