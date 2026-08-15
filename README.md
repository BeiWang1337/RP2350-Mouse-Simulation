# RP2350-Mouse-Simulation
> **Language / 语言:** **English (current)** | [中文](./readme_CN.md)

A dual-core, dual-role USB firmware for the **Raspberry Pi RP2350 (pico2, ARM-S core)**:
**Core 1 acts as a USB host** reading the physical mouse, **Core 0 acts as a USB device** enumerating to the PC as a **1:1 replica of the ATK Wireless Mouse 8K dongle**, with report sanitizing, anti-drop aggregation and hardware-level recoil control (RCS) in between.
No driver installation is required on the PC — it is recognized as a genuine ATK receiver as soon as it is plugged in.

---

## 1. Hardware Connection (the two USB ports must not be swapped)

| Port | Connects to | Notes |
|---|---|---|
| **Device port** (main USB) | → PC | Enumerates as the ATK dongle with 4 HID interfaces |
| **Host port** (PIO USB, VBUS_EN = GPIO18) | ← physical mouse | RP2350 acts as USB host reading raw mouse reports |

**Status LED** (no screen required — the st7735 display has been removed):

| Blink period | Meaning |
|---|---|
| 250 ms | Not enumerated / not mounted |
| 1000 ms | Mounted on PC **and** physical mouse mounted (normal) |
| 2500 ms | USB suspended |
| 50 ms | Initializing |

---

## 2. ATK 1:1 Emulation Details

### 2.1 Device Descriptor (`usb_descriptors.c` / `tusb_config.h`)

| Item | Value |
|---|---|
| VID / PID | `0x373B` / `0x1155` |
| Manufacturer | `ATK` |
| Product / Serial | `Wireless mouse 8k dongle-L1` |
| bcdDevice | `0x0101` |
| Config descriptor | 116 bytes, 4 HID interfaces |

### 2.2 HID Interface Layout (1:1 replica of ATK captures)

| Interface | Index | Protocol | Report descriptor | Endpoint | Polling |
|---|---|---|---|---|---|
| Keyboard | 0 | Boot Keyboard | 65 bytes | EP `0x81` IN, 8B | 8 ms |
| Mouse | 1 | Mouse | 79 bytes (Report ID `0x02`, **16-bit axes**) | EP `0x82` IN, 8B | 1 ms |
| Advanced | 2 | None | 121 bytes (Report ID 0x01~0x05, incl. 63B Feature) | EP `0x83` IN, 16B | 8 ms |
| Vendor | 3 | None | 36 bytes (Usage Page `0xFF05`, Report ID `0x08`, 63B IN/OUT) | EP `0x84` IN / `0x04` OUT, 64B | 8 ms |

### 2.3 Mouse Report Format (Interface 1, identical to ATK)

```
[0] Report ID 0x02
[1] buttons        (bit0=left bit1=right bit2=middle bit3=side1 bit4=side2)
[2..3] x           int16 little-endian, 16-bit relative axis
[4..5] y           int16 little-endian
[6] wheel          int8
```

The keyboard report is the standard 8-byte boot format without a report ID (modifier + reserved + 6 keys); LED output (Num/Caps/Scroll) is handled and echoed in `tud_hid_set_report_cb`.

---

## 3. Architecture & Data Flow

```
physical mouse ──USB Host(PIO)──► Core1 (tuh_hid_report_received_cb)
                                   │  parses 16-bit / 8-bit / long-packet /
                                   │  wireless-dongle report formats
                                   ▼
                          mouse_report_queue (64 deep, 8K-rate safe)
                                   │
                                   ▼ Core0 pump_usb_tasks()
                          ┌─ button-edge detection (flush frame instantly,
                          │  so rapid-fire macros are never swallowed)
   axis accumulator ──────┤─ wheel / side-button passthrough
                          └─ pack & send when endpoint is free
                                   │
                   RCS engine (optional) ── adds compensation into same frame
                                   │
                                   ▼
                USB Device ──► PC (enumerated as the ATK dongle)
```

- **Dual-core isolation**: Core 1 only receives and enqueues; Core 0 computes and sends. Float/interpolation work never blocks mouse reception interrupts.
- **Aggregator**: multiple physical reports are merged into one frame with strictly conserved movement (no lost deltas); any button-state change flushes the aggregation immediately so ultra-fast clicking is never merged away.
- **1:1 passthrough guarantee**: with RCS off (or left button released) the device is a pure forwarder — coordinates, buttons and wheel are output unchanged, behaving exactly like the mouse plugged in directly.

---

## 4. Hardware Recoil Control (RCS)

- Pattern table: `rcs_data.h`, 17 weapons (ak47 / m4a4 / m4a1 / galil / famas / sg553 / aug / p90 / bizon / ump45 / mac10 / mp5sd / mp7 / mp9 / m249 / negev / cz75).
- Engine: Q16.16 fixed-point math + Catmull-Rom spline interpolation + per-burst randomization (whole-curve ±1.5% scaling, ±0.5~0.75 px offset, progress jitter); compensation scale = `2.45 / game sensitivity`.
- Trigger: injects per-shot counter-movement while the left button is held and RCS is enabled; stops immediately on release.
- On-board controls: side button 1 (Mouse4) edge toggles RCS; the wheel cycles the weapon profile (original wheel events are still forwarded).

---

## 5. Host Control Protocol (Vendor Interface Report 0x08)

The upper computer (`host_hid2\Project2`) sends a **64-byte** Output report via `HidD_SetOutputReport`
(first byte = Report ID `0x08`, followed by 63 bytes of data). Local control frame format:

```
data[0..2] = 0xA5 0x5A 0x0A   // magic header (0x0A is the LOCAL_CMD_MACRO marker)
data[3]    = sub-command
```

| Sub-command | Meaning | Payload |
|---|---|---|
| `0x0C` | **Config sync** (weapon / sensitivity / enable) | `[4]`=weapon id (0..16) `[5..8]`=sensitivity float LE `[9]`=enable (0/1) |
| `0x0B` | Aim movement | `[4..5]`=dx `[6..7]`=dy (int16 LE) |
| `1` | Left-click macro | — |
| `4` | Side-1 click macro | — |
| `5` | Side-2 click macro | — |

Output reports on 0x08 that are **not** local control frames (not starting with `A5 5A 0A`) are treated as **bridge traffic** and forwarded to the physical mouse's vendor interface (Report ID 0x08), so the OEM driver / macro software keeps working with the physical mouse.

> Host flow: CS2 GSI detects the current weapon → 0x0C config frame → firmware switches
> the pattern; the web console at `http://127.0.0.1:52710` adjusts sensitivity / enable.
> Out-of-range weapon ids are reset to 0 and sensitivity is clamped to 0.01 ~ 20 by the firmware.

---

## 6. Build

Prerequisites: CMake ≥ 3.17, Ninja, arm-none-eabi GCC (e.g. xpack 14.2.1), pico-sdk 2.2.0, Python 3 (for UF2 conversion).

```bat
cd host_hid2
cmake -G Ninja -S . -B build2 ^
  -DPICO_SDK_PATH="C:/Program Files/pico-sdk-2.2.0" ^
  -DPICO_BOARD=pico2 ^
  -DPICO_PLATFORM=rp2350-arm-s ^
  -DFAMILY=rp2040 ^
  -DCMAKE_TOOLCHAIN_FILE="C:/Program Files/pico-sdk-2.2.0/cmake/preload/toolchains/pico_arm_cortex_m33_gcc.cmake"
ninja -C build2
```

Output: `build2\host_hid2.uf2` (~154 KB, display driver removed).

> Note: this TinyUSB fork only ships the `rp2040` BSP family directory; the RP2350 target
> is determined by `PICO_BOARD=pico2` + `PICO_PLATFORM=rp2350-arm-s` + the M33 toolchain.
> `FAMILY=rp2040` only selects the TinyUSB BSP layer — do not change it.

## 7. Flashing

Hold the RP2350 BOOT button while plugging it in, then drag `host_hid2.uf2` onto the mass-storage drive that appears (UF2 family ID `0xE48BFF59`, ARM-S specific).

---

## 8. Directory Layout

```
host_hid2/
├── CMakeLists.txt          # build entry (dual usb example)
├── tinyusb/                # TinyUSB (incl. Pico-PIO-USB host)
├── build2/                 # build output (host_hid2.uf2)
└── src/
    ├── main.c              # dual-core entry, task pump, aggregator, RCS engine, local control frames
    ├── usb_descriptors.c/h # ATK 1:1 device/config/string/HID report descriptors
    ├── hid_reports.c/h     # keyboard/mouse report packing (ATK 7-byte format)
    ├── rcs_core.c/h        # recoil core (legacy standalone implementation, kept)
    ├── rcs_data.h          # 17 weapon recoil patterns
    └── tusb_config.h       # TinyUSB config + ATK identity (VID/PID/strings)
```

---

## 9. FAQ

| Symptom | Check |
|---|---|
| No response / mouse not working | The two USB ports are swapped; when wired correctly the LED blinks at a 1 s period |
| No recoil while holding left button | RCS is disabled (side-button toggle / host sent enable=0); check the host `[HID] ... rcs=on` log |
| Wrong compensation strength | The host "game sensitivity" must equal the in-game CS2 sensitivity; smaller value = stronger compensation, larger = weaker (compensation = 2.45/sens) |
| Weapon does not switch | GSI not active: start the host app before the game (cfg files load at game launch); confirm `[GSI] active=...` appears in the console |
| OEM driver / macro software cannot reach the mouse | The vendor-interface bridge only supports the Report 0x08 path; confirm the mouse's vendor report id is 0x08 |
