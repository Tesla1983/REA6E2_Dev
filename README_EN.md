> Other languages: [中文](README.md)

# RA6E2 System Monitor Display

A "PC system monitor peripheral" built on a Renesas RA6E2 development board: a 128×64
monochrome dot-matrix screen shows your PC's live CPU / GPU temperature, utilization, fan
speed, and memory usage in real time. The data is gathered by a Windows host app and sent
down over USB CDC (virtual serial).

Think of it as a small hardware dashboard for your desk that mirrors your PC's live stats.

---

## Architecture

```
┌─────────────────────────┐        USB CDC (virtual serial)       ┌──────────────────────────┐
│     Windows PC          │ ───────────────────────────────────▶ │   RA6E2 board            │
│  ra6e2-monitor          │                                       │   bare-metal FW (FSP 6.6)│
│  (Rust + egui)          │                                       │   NT7534 128×64 render   │
│  collect CPU/GPU/mem    │                                       │                          │
└─────────────────────────┘                                       └──────────────────────────┘
       PDH / DXGI / LHM                                               (Cortex-M33 @ 200MHz)
```

The host app does the "collect + send"; the board does the "receive + render". The two are
decoupled by a simple text protocol.

---

## Hardware

| Item | Description |
| --- | --- |
| Board | Diqixing RA6E2 (R7FA6E2BB3CNE, QFN48) |
| MCU | Renesas RA6E2, ARM Cortex-M33, 200 MHz, 256 KB Flash |
| Display | 128×64 monochrome dot-matrix LCD, NT7534 controller (bit-banged SPI) |
| Flash/Debug | FireDAP (CMSIS-DAP, VID:PID = `0416:5021`), SWD |

Pin assignment:

| Peripheral | Pins |
| --- | --- |
| USB CDC | D+ = P814, D- = P815, VBUS = P407 |
| NT7534 display | RS = P403, RES = P409, CLK = P302, SID = P207, CS = P301 |
| Status LED | P402 |
| SWD | SWCLK = P108, SWDIO = P300 |

---

## Features

### Firmware (RA6E2 side)

- Live display of **CPU temp / utilization / fan**, **GPU temp / utilization / fan**, and
  **memory usage** (tabular layout).
- Dual 3-blade fan animation at bottom-right: spins only while linked, speed follows the
  actual RPM; on disconnect the fans freeze but stay visible.
- Frame rate is **decoupled** from the host's send rate (independent time base): numbers
  refresh at ~1 Hz, screen redraws at ~60 fps with partial dirty-region updates.
- USB CDC link with `$ID?` identity query (returns the chip's factory UID + firmware
  version), so the host recognizes the board by **chip UID** instead of the COM port
  (which changes on every replug).
- Keeps the last readings on disconnect.
- Current firmware version: **v24** (`0x4E540018`).

### Host app (ra6e2-monitor, Windows)

- Rust + egui desktop app that collects local CPU / GPU / memory metrics (PDH, DXGI,
  LibreHardwareMonitor).
- Sends data to the board over USB CDC; auto-detects the target board by chip UID.
- Dark theme; remembers window zoom, position, and size (`ui.json`, portable mode supported).
- System tray: clicking the window **X = quit**; only the "minimize to tray" button hides it.
- GPU VRAM usage card (off by default).
- Fan unit auto-adapts: sends RPM when available, otherwise `%` (UI shows `%`).
- Shows readings even when not connected (bright green).

---

## Communication Protocol

Host → firmware, one line per frame, terminated by `CRLF`, with an 8-bit XOR checksum:

```
$CT:..,CU:..,CF:..,GT:..,GU:..,GF:..,MU:..,GV:..*XX\r\n
```

| Field | Meaning | Unit |
| --- | --- | --- |
| `CT` | CPU temperature | °C |
| `CU` | CPU utilization | % |
| `CF` | CPU fan speed | RPM |
| `GT` | GPU temperature | °C |
| `GU` | GPU utilization | % |
| `GF` | GPU fan speed | RPM |
| `MU` | Memory usage | % |
| `GV` | GPU VRAM usage | % (not yet consumed by FW v24, reserved) |

- `XX` is the 8-bit XOR checksum (hex) over the payload between `$` and `*`.
- Missing fields (e.g. a fan RPM that can't be read) are sent as `-1`; the firmware treats
  them as absent and shows `--` on screen.
- Identity query: the host sends `$ID?*XX\r\n`; the firmware replies
  `$ID:RA6E2-<UID>,FW:<ver>*XX\r\n` (`FW` is the firmware minor version, e.g. `24`).

---

## Build & Flash

### Firmware

Dependencies: Renesas FSP 6.6.0, Arm GNU Toolchain (GCC), CMake + Ninja, RASC (code
generation), probe-rs.

```bash
# 1) Open configuration.xml in RASC and generate code for GCC + R7FA6E2BB (if not done yet)
# 2) Build
./scripts/build.sh                 # output: build/ra6e2_fw.elf
# 3) Flash (FireDAP, no auto-reset by default)
./scripts/flash.sh --speed 1000 --verify
# 4) To run the firmware: physically unplug & replug the Type-C (cold boot)
```

> The flash script does **not** reset the core by default (otherwise the enumerated USB CDC
> port would disappear). After flashing, unplug and replug the Type-C to start the firmware.

### Host app

Dependencies: Rust toolchain (edition 2024), Windows (metrics use Windows APIs).

```bash
cd gui
cargo build --release             # output: gui/target/release/ra6e2-monitor.exe
```

> Some performance counters require running as **Administrator**.

---

## Usage

1. Flash and power the RA6E2, connect it to the PC via Type-C (a CDC serial port appears).
2. Run `ra6e2-monitor.exe`; it enumerates ports, recognizes the board by UID, and connects.
3. Your PC's live metrics show up on the board's screen.

---

## Directory Layout

| Path | Content |
| --- | --- |
| `src/` | Firmware source (NT7534 driver, USB CDC, data-link parser) |
| `gui/` | Host application (Rust + egui) |
| `scripts/` | Build / flash scripts |
| `docs/` | Dev-environment, display, USB CDC docs (incl. `3.5寸屏开发文档.md`) |
| `vendor/`, `third_party/` | FSP and third-party components |
| `host/`, `tools/` | Helper scripts and tools |

---

## License

This project currently ships **without a LICENSE file**. Add an appropriate license before
publishing.
