# host_hid2 — RP2350 ATK 鼠标 1:1 仿真固件

> **Language / 语言:** [English](./readme.md) | **中文（当前）**

基于 **Raspberry Pi RP2350（pico2, ARM-S 核心）** 的双核双角色 USB 固件：
**Core 1 当 USB 主机**读取物理鼠标，**Core 0 当 USB 设备**以 **1:1 复刻的 ATK 无线鼠标 8K 接收器**身份枚举到电脑，中间做数据清洗、防丢帧聚合与硬件级压枪（RCS）。
无需在电脑上安装任何驱动，插上即被系统识别为原厂 ATK 鼠标接收器。
上传并无附带RCS源码
---

## 1. 硬件连接（两个 USB 口不能插反）

| 接口 | 接什么 | 说明 |
|---|---|---|
| **Device 口**（主 USB） | → 电脑 | 枚举为 ATK 接收器，提供 4 个 HID 接口 |
| **Host 口**（PIO USB，VBUS_EN = GPIO18） | ← 物理鼠标 | RP2350 作为 USB Host 读取鼠标原始报告 |

**LED 状态灯**（无需屏幕，st7735 显示已移除）：

| 闪烁周期 | 含义 |
|---|---|
| 250 ms | 未枚举 / 未挂载 |
| 1000 ms | 设备已挂载到电脑 且 物理鼠标已挂载（正常状态） |
| 2500 ms | USB 挂起 |
| 50 ms | 初始化阶段 |

---

## 2. ATK 1:1 仿真细节

### 2.1 设备描述符（`usb_descriptors.c` / `tusb_config.h`）

| 项 | 值 |
|---|---|
| VID / PID | `0x373B` / `0x1155` |
| 制造商 | `ATK` |
| 产品名 / 序列号 | `Wireless mouse 8k dongle-L1` |
| bcdDevice | `0x0101` |
| 配置描述符总长 | 116 字节，4 个 HID 接口 |

### 2.2 HID 接口布局（1:1 还原 ATK 抓包）

| 接口 | 索引 | 协议 | 报告描述符 | 端点 | 轮询 |
|---|---|---|---|---|---|
| 键盘 | 0 | Boot Keyboard | 65 字节 | EP `0x81` IN, 8B | 8 ms |
| 鼠标 | 1 | Mouse | 79 字节（Report ID `0x02`，**16 位坐标**） | EP `0x82` IN, 8B | 1 ms |
| 高级扩展 | 2 | None | 121 字节（Report ID 0x01~0x05，含 63B Feature） | EP `0x83` IN, 16B | 8 ms |
| 厂商通信 | 3 | None | 36 字节（Usage Page `0xFF05`，Report ID `0x08`，63B IN/OUT） | EP `0x84` IN / `0x04` OUT, 64B | 8 ms |

### 2.3 鼠标报告格式（Interface 1，与 ATK 完全一致）

```
[0] Report ID 0x02
[1] buttons        (bit0=左键 bit1=右键 bit2=中键 bit3=侧键1 bit4=侧键2)
[2..3] x           int16 小端, 16 位相对坐标
[4..5] y           int16 小端
[6] wheel          int8
```

键盘报告为无 ID 的 8 字节标准 Boot 格式（修饰键 + 保留 + 6 键位），LED 输出（Num/Caps/Scroll）在 `tud_hid_set_report_cb` 中处理并回写。

---

## 3. 架构与数据流

```
物理鼠标 ──USB Host(PIO)──► Core1 (tuh_hid_report_received_cb)
                              │  解析 16位/8位/长包/无线 dongle 各种格式
                              ▼
                     mouse_report_queue (64 深, 防 8K 溢出)
                              │
                              ▼ Core0 pump_usb_tasks()
                     ┌─ 按键边沿检测(立即发包, 防宏吞帧)
  坐标聚合器(accum) ─┤─ 滚轮/侧键透传
                     └─ 端点空闲时打包 → send_mouse_report()
                              │
              RCS 压枪引擎(可选) ── 补偿位移叠加进同一帧
                              │
                              ▼
           USB Device ──► 电脑 (枚举为 ATK 接收器)
```

- **双核隔离**：Core 1 只负责收包入队，Core 0 负责计算与发送，浮点/插值运算不阻塞鼠标接收中断。
- **聚合器**：多个物理报告合并为一帧发送，位移严格守恒不丢帧；检测到按键状态变化立刻截断聚合并发包，保证极速连点不被吞。
- **1:1 透传保证**：RCS 关闭时（或未按左键时）设备就是纯转发，坐标、按键、滚轮原样输出，鼠标行为与直插电脑一致。

---

## 4. 硬件级压枪引擎（RCS）

- 弹道表：`rcs_data.h`，17 把武器（ak47 / m4a4 / m4a1 / galil / famas / sg553 / aug / p90 / bizon / ump45 / mac10 / mp5sd / mp7 / mp9 / m249 / negev / cz75）。
- 引擎：Q16.16 定点运算 + Catmull-Rom 样条插值 + 段内随机扰动（曲线整体 ±1.5% 缩放、±0.5~0.75px 偏移、进度抖动），补偿幅度 = `2.45 / 游戏灵敏度`。
- 触发：按住左键 + RCS 开启时逐发注入；松开立即停止。
- 板载控制：侧键 1（Mouse4）边沿切换 RCS 开关；滚轮循环切换武器配置（会保留原始滚轮事件透传）。

---

## 5. 上位机控制协议（Vendor 接口 Report 0x08）

上位机（`host_hid2\Project2`）通过 `HidD_SetOutputReport` 发送 **64 字节** Output 报表
（首字节 = Report ID `0x08`，后接 63 字节数据），本地控制帧格式：

```
数据[0..2] = 0xA5 0x5A 0x0A   // 魔术头（0x0A 即 LOCAL_CMD_MACRO 标记）
数据[3]    = 子命令
```

| 子命令 | 含义 | 载荷 |
|---|---|---|
| `0x0C` | **配置同步**（武器/灵敏度/开关） | `[4]`=武器ID(0..16) `[5..8]`=灵敏度 float 小端 `[9]`=开关(0/1) |
| `0x0B` | 瞄准移动 | `[4..5]`=dx `[6..7]`=dy (int16 小端) |
| `1` | 左键点击宏 | — |
| `4` | 侧键 1 点击宏 | — |
| `5` | 侧键 2 点击宏 | — |

非本地控制帧（不是 `A5 5A 0A` 开头）的 0x08 Output 报表会被当作**桥接流量**透传给物理鼠标的厂商接口（Report ID 0x08），保证原厂驱动/宏软件对物理鼠标的配置照常工作。

> 上位机流程：CS2 GSI 检测当前武器 → 0x0C 配置帧 → 固件切换弹道；网页控制台
> `http://127.0.0.1:52710` 可调灵敏度/开关。固件收到越界武器 ID 自动归零，
> 灵敏度钳制在 0.01 ~ 20。

---

## 6. 编译

前置：CMake ≥ 3.17、Ninja、arm-none-eabi GCC（如 xpack 14.2.1）、pico-sdk 2.2.0、Python 3（UF2 转换用）。

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

产物：`build2\host_hid2.uf2`（约 154 KB，已去除屏幕驱动）。

> 说明：本工程的 TinyUSB 分支只有 `rp2040` BSP 家族目录，RP2350 由
> `PICO_BOARD=pico2` + `PICO_PLATFORM=rp2350-arm-s` + M33 工具链决定；
> `FAMILY=rp2040` 仅用于选择 TinyUSB 的 BSP 层，请勿改成其它值。

## 7. 烧录

按住 RP2350 的 BOOT 键接入电脑，出现 U 盘后把 `host_hid2.uf2` 拖进去即可（UF2 family ID `0xE48BFF59`，ARM-S 专用）。

---

## 8. 目录结构

```
host_hid2/
├── CMakeLists.txt          # 构建入口 (dual usb example)
├── tinyusb/                # TinyUSB (含 Pico-PIO-USB host)
├── build2/                 # 构建输出 (host_hid2.uf2)
└── src/
    ├── main.c              # 双核入口、任务泵、聚合器、RCS 引擎、本地控制帧处理
    ├── usb_descriptors.c/h # ATK 1:1 设备/配置/字符串/HID 报告描述符
    ├── hid_reports.c/h     # 键盘/鼠标报告封装 (ATK 7 字节格式)
    ├── rcs_core.c/h        # 压枪核心 (旧版独立实现, 保留)
    ├── rcs_data.h          # 17 把武器弹道数据
    └── tusb_config.h       # TinyUSB 配置 + ATK 伪装参数 (VID/PID/字符串)
```

---

## 9. 常见问题

| 现象 | 排查 |
|---|---|
| 插上没反应 / 鼠标不通 | 两个 USB 口插反；正确连接后 LED 应为 1s 周期闪烁 |
| 按住左键不压枪 | RCS 被关闭（侧键切换 / 上位机下发开关=0）；检查上位机 `[HID] ... rcs=on` 日志 |
| 压枪幅度不对 | 上位机"游戏灵敏度"必须等于 CS2 游戏内灵敏度；数值调小补偿变强、调大变弱（补偿 = 2.45/sens） |
| 换武器不切换 | GSI 未生效：先启动上位机再启动游戏（cfg 仅开局加载）；确认控制台有 `[GSI] active=...` 输出 |
| 原厂驱动/宏软件连不上鼠标 | 厂商接口桥接仅支持 Report 0x08 路径；确认鼠标的厂商接口报告 ID 为 0x08 |
