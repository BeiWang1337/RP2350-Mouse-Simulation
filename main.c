#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "bsp/board_api.h"
#include "device/usbd_pvt.h"
#include "tusb.h"

#include "hid_reports.h"
#include "usb_descriptors.h"
#include "rcs_data.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"

volatile uint8_t g_current_weapon_id = 0;
volatile float g_game_sensitivity = 0.75f;
volatile bool g_rcs_enabled = true;

enum {
  BLINK_INIT_MOUNTED = 50, BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000, BLINK_SUSPENDED = 2500,
};
static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;
void led_blinking_task(void);

typedef struct {
  uint8_t dev_addr;
  uint8_t idx;
  uint8_t protocol;
  uint8_t mouse_report_id;
  bool mouse_16bit_axes;
  uint8_t mouse_wire_format;
  bool mouse_interface;
  bool vendor_interface;
  bool feature_interface;
  uint8_t vendor_report_id;
} hid_device_t;
hid_device_t hid_devices[CFG_TUH_HID];

typedef struct {
  uint8_t modifier;   
  uint8_t keycode[6]; 
} keyboard_report_t;

// 高级系统/多媒体按键结构体（用于接收侧键）
typedef struct {
    uint8_t report_id;
    uint8_t len;
    uint8_t data[16];
} advanced_report_t;

#define BRIDGE_REPORT_MAX_LEN 64
#define LOCAL_CMD_MAGIC0 0xA5u
#define LOCAL_CMD_MAGIC1 0x5Au
#define LOCAL_CMD_MACRO 0x0Au
#define LOCAL_CMD_CONFIG 0x0Cu /* 上位机配置: 武器ID + 灵敏度 + 总开关 */
typedef struct {
  uint8_t target;
  uint8_t report_id;
  uint8_t report_type;
  uint8_t len;
  uint8_t interrupt_out;
  uint8_t data[BRIDGE_REPORT_MAX_LEN];
} bridge_report_t;

hid_mouse_state_t g_phys_mouse = {0};

#define ATK_FEATURE_REPORT_LEN 63
#define ATK_VENDOR_REPORT_09_LEN 48
#define ATK_VENDOR_REPORT_0A_LEN 48

static uint8_t g_keyboard_leds = 0;
static uint8_t g_feature_report_03[ATK_FEATURE_REPORT_LEN];
static uint8_t g_feature_report_04[ATK_FEATURE_REPORT_LEN];
static uint8_t g_vendor_report_08[ATK_FEATURE_REPORT_LEN];
static uint8_t g_vendor_report_09[ATK_VENDOR_REPORT_09_LEN];
static uint8_t g_vendor_report_0A[ATK_VENDOR_REPORT_0A_LEN];

static volatile bool g_usb_suspended = false;
static volatile bool g_remote_wakeup_allowed = false;

// ★ 核心：鼠标缓冲队列放大，完美承接 8K 回报率防溢出
#define KEYBOARD_QUEUE_SIZE 8
#define MOUSE_QUEUE_SIZE 64  
#define ADVANCED_QUEUE_SIZE 8 
#define VENDOR_QUEUE_SIZE 8

static queue_t keyboard_report_queue;
static queue_t mouse_report_queue;
static queue_t advanced_report_queue;
static queue_t vendor_input_queue;
static queue_t bridge_output_queue;

static volatile uint32_t g_mouse_queue_drops = 0;

static bool queue_mouse_report(hid_mouse_state_t const *report)
{
    if (report != NULL && queue_try_add(&mouse_report_queue, report)) {
        return true;
    }
    ++g_mouse_queue_drops;
    return false;
}

static int16_t clamp_mouse_axis(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static bool g_mouse_host_valid = false;
static uint8_t g_mouse_host_dev_addr = 0;
static uint8_t g_mouse_host_instance = 0;
static bool g_vendor_host_valid = false;
static uint8_t g_vendor_host_dev_addr = 0;
static uint8_t g_vendor_host_instance = 0;
static bool g_feature_host_valid = false;
static uint8_t g_feature_host_dev_addr = 0;
static uint8_t g_feature_host_instance = 0;
static bool g_bridge_control_busy = false;
static bridge_report_t g_bridge_inflight;
typedef struct {
  volatile bool pending;
  volatile bool busy;
  volatile bool done;
  uint8_t target;
  uint8_t report_id;
  uint8_t report_type;
  uint16_t request_len;
  volatile uint16_t actual_len;
  uint8_t data[BRIDGE_REPORT_MAX_LEN];
} get_report_bridge_t;
static get_report_bridge_t g_get_report_bridge;
static uint8_t g_vendor_last_input[BRIDGE_REPORT_MAX_LEN];
static uint8_t g_vendor_last_input_len = 0;
static uint8_t g_vendor_last_input_id = 0;

// ★ 核心：超高频坐标累加器，杜绝丢帧卡顿
static int32_t accum_dx = 0;
static int32_t accum_dy = 0;
static int32_t accum_wheel = 0;
static uint8_t accum_last_buttons = 0xFF;
static int32_t pending_rcs_dx = 0;
static int32_t pending_rcs_dy = 0;
static uint8_t control_previous_buttons = 0;

static void process_mouse_controls(const hid_mouse_state_t *mouse)
{
    if (mouse == NULL) return;

    const uint8_t weapon_count = (uint8_t)(sizeof(ALL_WEAPONS) /
                                            sizeof(ALL_WEAPONS[0]));

    // Toggle once on the upper side-button press edge, not while held.
    bool side_pressed = (mouse->buttons & MOUSE_BUTTON_4) != 0;
    bool side_was_pressed = (control_previous_buttons & MOUSE_BUTTON_4) != 0;
    if (side_pressed && !side_was_pressed) {
        g_rcs_enabled = !g_rcs_enabled;
    }

    // Cycle through the profiles while preserving the original wheel event.
    if (mouse->wheel != 0 && weapon_count != 0) {
        if (g_current_weapon_id >= weapon_count) {
            g_current_weapon_id = 0;
        }
        if (mouse->wheel > 0) {
            g_current_weapon_id = (uint8_t)((g_current_weapon_id + 1u) %
                                            weapon_count);
        } else {
            g_current_weapon_id = g_current_weapon_id == 0
                ? (uint8_t)(weapon_count - 1u)
                : (uint8_t)(g_current_weapon_id - 1u);
        }
    }

    // Controls are intentionally not consumed; the report is forwarded as-is.
    control_previous_buttons = mouse->buttons;
}

static bool descriptor_has_16bit_relative_axes(uint8_t const *desc,
                                               uint16_t len)
{
    static const uint8_t pattern_zero_min[] = {
        0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0x80, 0x26,
        0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06
    };
    static const uint8_t pattern_one_min[] = {
        0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26,
        0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06
    };

    if (desc == NULL || len < sizeof(pattern_zero_min)) return false;

    for (uint16_t i = 0; i <= len - sizeof(pattern_zero_min); ++i) {
        if (memcmp(&desc[i], pattern_zero_min, sizeof(pattern_zero_min)) == 0 ||
            memcmp(&desc[i], pattern_one_min, sizeof(pattern_one_min)) == 0) {
            return true;
        }
    }
    return false;
}

static bool descriptor_has_vendor_64(uint8_t const *desc, uint16_t len)
{
    if (desc == NULL || len < 7u) return false;

    for (uint16_t i = 0; i + 6u < len; ++i) {
        // The Zero wireless dongle uses FF00/FF01 on MI_02 and FF05 on
        // MI_03. All of these are vendor transport collections with the
        // same 64-byte endpoint packet size.
        if (desc[i] != 0x06 || desc[i + 2u] != 0xFF ||
            desc[i + 3u] != 0x09 || desc[i + 4u] != 0x01 ||
            desc[i + 5u] != 0xA1 || desc[i + 6u] != 0x01) {
            continue;
        }

        bool has_input = false;
        bool has_output = false;
        bool has_64_byte_report = false;
        for (uint16_t j = (uint16_t)(i + 7u);
             j + 2u < len && j < i + 80u; ++j) {
            if (desc[j] == 0x95 &&
                (desc[j + 1u] == 0x40 || desc[j + 1u] == 0x3F)) {
                has_64_byte_report = true;
            }
            if (has_64_byte_report && desc[j] == 0x81) has_input = true;
            if (has_64_byte_report && desc[j] == 0x91) has_output = true;
            if (desc[j] == 0xC0) break;
        }
        if (has_input && has_output) return true;
    }
    return false;
}

static bool descriptor_has_feature_reports(uint8_t const *desc, uint16_t len)
{
    if (desc == NULL || len < 8u) return false;

    bool has_feature = false;
    bool has_feature_id = false;
    for (uint16_t i = 0; i + 1u < len; ++i) {
        if (desc[i] == 0x85 &&
            (desc[i + 1u] == 0x03 || desc[i + 1u] == 0x04)) {
            has_feature_id = true;
        }
        if (desc[i] == 0xB1 && desc[i + 1u] == 0x02) has_feature = true;
    }
  return has_feature_id && has_feature;
}

// Some wireless dongles expose the mouse interface with protocol NONE.
// Identify it from the report descriptor instead of dropping its reports.
static bool descriptor_has_mouse_usage(uint8_t const *desc, uint16_t len)
{
    if (desc == NULL || len < 4u) return false;

    for (uint16_t i = 0; i + 3u < len; ++i) {
        if (desc[i] == 0x05 && desc[i + 1u] == 0x01 &&
            desc[i + 2u] == 0x09 && desc[i + 3u] == 0x02) {
            return true;
        }
    }
    return false;
}

static uint8_t descriptor_first_report_id(uint8_t const *desc, uint16_t len)
{
    if (desc == NULL) return 0;

    for (uint16_t i = 0; i + 1u < len; ++i) {
        if (desc[i] == 0x85) return desc[i + 1u];
    }
    return 0;
}

static int16_t read_le_i16(uint8_t const *data)
{
    uint16_t value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return (int16_t)value;
}

static bool parse_mouse_report(uint8_t instance, uint8_t const *report,
                               uint16_t len, hid_mouse_state_t *out)
{
    if (report == NULL || out == NULL || instance >= CFG_TUH_HID) {
        return false;
    }

    hid_device_t *device = &hid_devices[instance];

    // Follow the report descriptor. The real Zero mouse uses Report ID 0x02.
    if (device->mouse_16bit_axes) {
        uint8_t offset = 0;
        if (device->mouse_report_id != 0) {
            if (len < 7u || report[0] != device->mouse_report_id) {
                return false;
            }
            offset = 1u;
        }
        if (len < offset + 6u) return false;
        out->buttons = (uint8_t)(report[offset] & 0x1Fu);
        out->x = read_le_i16(&report[offset + 1u]);
        out->y = read_le_i16(&report[offset + 3u]);
        out->wheel = (int8_t)report[offset + 5u];
        return true;
    }

    // Standard boot mouse fallback: buttons, relative X/Y, and vertical wheel.
    uint8_t offset = 0;
    if (device->mouse_report_id != 0) {
        if (len < 5u || report[0] != device->mouse_report_id) return false;
        offset = 1u;
    }
    uint16_t data_len = (uint16_t)(len - offset);
    if (data_len < 3u) return false;
    uint8_t buttons = (uint8_t)(report[offset] & 0x1Fu);
    out->buttons = buttons;
    out->x = (int8_t)report[offset + 1u];
    out->y = (int8_t)report[offset + 2u];
    out->wheel = data_len >= 4u ? (int8_t)report[offset + 3u] : 0;
    return true;
}

// =====================================================================
// 核心任务泵浦函数
// =====================================================================
void pump_usb_tasks(int16_t rcs_dx, int16_t rcs_dy, uint64_t target_us) {
    pending_rcs_dx += rcs_dx;
    pending_rcs_dy += rcs_dy;
    bool rcs_sent = (pending_rcs_dx == 0 && pending_rcs_dy == 0);
    // Keep the legacy final-drain condition active for deferred RCS data.
    if (!rcs_sent && rcs_dx == 0 && rcs_dy == 0) rcs_dx = 1;
    
    do {
        tud_task(); 

        bridge_report_t vendor_input;
        if (tud_hid_n_ready(ITF_NUM_VENDOR) &&
            queue_try_remove(&vendor_input_queue, &vendor_input)) {
            if (!tud_hid_n_report(ITF_NUM_VENDOR, vendor_input.report_id,
                                  vendor_input.data, vendor_input.len)) {
                queue_try_add(&vendor_input_queue, &vendor_input);
            }
        }

        if (g_usb_suspended && g_remote_wakeup_allowed &&
            (!queue_is_empty(&keyboard_report_queue) ||
             !queue_is_empty(&mouse_report_queue) ||
             !queue_is_empty(&advanced_report_queue))) {
            tud_remote_wakeup();
        }
        
        keyboard_report_t kbd;
        if (tud_hid_n_ready(ITF_NUM_KEYBOARD) && queue_try_remove(&keyboard_report_queue, &kbd)) {
            send_keyboard_report(kbd.modifier, kbd.keycode);
        }

        // 抽空队列，累积物理鼠标生成的所有位移
        hid_mouse_state_t phys;
        while (queue_try_remove(&mouse_report_queue, &phys)) {
            process_mouse_controls(&phys);
            bool btn_changed = (g_phys_mouse.buttons != phys.buttons); // 捕捉边缘变化
            g_phys_mouse.buttons = phys.buttons;
            if (!(g_phys_mouse.buttons & MOUSE_BUTTON_LEFT)) {
                pending_rcs_dx = 0;
                pending_rcs_dy = 0;
            }
            accum_dx += phys.x;
            accum_dy += phys.y;
            accum_wheel += phys.wheel;
            
            // ★ 核心修复：如果检测到按键状态变化，立刻中断聚合！
            // 强制让端点发送这一帧，防止宏按键（极速连点）被合并吞噬。
            if (btn_changed) {
                break;
            }
        }

        // 当端点空闲时，打包发送
        if (tud_hid_n_ready(ITF_NUM_MOUSE)) {
            bool need_send = false;
            if (accum_dx != 0 || accum_dy != 0 ||
                accum_wheel != 0) need_send = true;
            if (g_phys_mouse.buttons != accum_last_buttons) need_send = true;
            if (!rcs_sent) need_send = true;

            if (need_send) {
                int32_t send_dx = accum_dx;
                int32_t send_dy = accum_dy;
                
                if (!rcs_sent) {
                    send_dx += pending_rcs_dx;
                    send_dy += pending_rcs_dy;
                    pending_rcs_dx = 0;
                    pending_rcs_dy = 0;
                    rcs_sent = true;
                }

                // 添加 (int8_t) 强转以消除编译警告
                int8_t send_wheel = (int8_t)((accum_wheel > 127) ? 127 : ((accum_wheel < -128) ? -128 : accum_wheel));
                send_mouse_report(g_phys_mouse.buttons,
                                  clamp_mouse_axis(send_dx),
                                  clamp_mouse_axis(send_dy),
                                  send_wheel, 0);
                
                accum_dx = 0; accum_dy = 0; accum_wheel = 0;
                accum_last_buttons = g_phys_mouse.buttons;
            } else if (!rcs_sent) {
                rcs_sent = true; 
            }
        }

        // 转发高级多媒体键
        if (tud_hid_n_ready(ITF_NUM_ADVANCED)) {
            advanced_report_t adv;
            if (queue_try_remove(&advanced_report_queue, &adv)) {
                tud_hid_n_report(ITF_NUM_ADVANCED, adv.report_id, adv.data, adv.len);
            }
        }

    } while (time_us_64() < target_us);

    // 循环退出收尾
    if (!rcs_sent && (rcs_dx != 0 || rcs_dy != 0)) {
        uint64_t wait_deadline = time_us_64() + 2000u;
        while (!tud_hid_n_ready(ITF_NUM_MOUSE) &&
               time_us_64() < wait_deadline) {
            tud_task();
        }
        if (!tud_hid_n_ready(ITF_NUM_MOUSE)) return;
        
        hid_mouse_state_t p;
        while (queue_try_remove(&mouse_report_queue, &p)) {
            process_mouse_controls(&p);
            g_phys_mouse.buttons = p.buttons;
            if (!(g_phys_mouse.buttons & MOUSE_BUTTON_LEFT)) {
                pending_rcs_dx = 0;
                pending_rcs_dy = 0;
            }
            accum_dx += p.x; accum_dy += p.y;
            accum_wheel += p.wheel;
        }
        
        int32_t send_dx = accum_dx + pending_rcs_dx;
        int32_t send_dy = accum_dy + pending_rcs_dy;
        
        // 添加 (int8_t) 强转以消除编译警告
        int8_t send_wheel = (int8_t)((accum_wheel > 127) ? 127 : ((accum_wheel < -128) ? -128 : accum_wheel));
        send_mouse_report(g_phys_mouse.buttons,
                          clamp_mouse_axis(send_dx),
                          clamp_mouse_axis(send_dy),
                          send_wheel, 0);
        
        accum_dx = 0; accum_dy = 0; accum_wheel = 0;
        pending_rcs_dx = 0; pending_rcs_dy = 0;
        accum_last_buttons = g_phys_mouse.buttons;
    }
}

// =====================================================================
// 物理级硬件压枪引擎
//
// 曲线表仍然使用浮点数保存，运行时的累计、舍入和时间调度使用
// Q16.16 定点数。这样每个细分点只会在最终量化处产生一次误差，
// 并且小数余量会均匀分布到所有细分点，而不是集中到最后一帧。
// =====================================================================
#define RCS_Q_SHIFT 16
#define RCS_Q_ONE (1L << RCS_Q_SHIFT)
#define RCS_SENS_MULT_Q16 160563L       // round(2.45 * 65536)
#define RCS_JITTER_AMPLITUDE_Q16 2048L  // 3.125% 的进度扰动，端点为零
#define RCS_CURVE_SCALE_VARIATION_Q16 983L  // 每次曲线约 +/-1.5%
#define RCS_CURVE_OFFSET_X_MAX_Q16 49152L  // 整条曲线横向最多约 +/-0.75px
#define RCS_CURVE_OFFSET_Y_MAX_Q16 32768L  // 整条曲线纵向最多约 +/-0.50px
#define RCS_MAX_PATTERN_POINTS 192

enum {
    RCS_PROFILE_COUNT = sizeof(ALL_WEAPONS) / sizeof(ALL_WEAPONS[0])
};
static int32_t rcs_cached_path_x[RCS_PROFILE_COUNT][RCS_MAX_PATTERN_POINTS];
static int32_t rcs_cached_path_y[RCS_PROFILE_COUNT][RCS_MAX_PATTERN_POINTS];
static bool rcs_path_cache_ready = false;

static uint32_t rcs_rng_state = 0xA341316Cu;
static uint32_t rcs_sequence_counter = 0u;

static uint32_t rcs_next_random(void) {
    // xorshift32 比 libc rand() 更轻量，也不会因为固定启动状态反复生成
    // 同一条扰动序列。
    uint32_t value = rcs_rng_state;
    if (value == 0u) value = 0x6D2B79F5u;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    rcs_rng_state = value;
    return value;
}

static int32_t rcs_random_signed_q16(int32_t amplitude_q16) {
    int32_t noise = (int32_t)(rcs_next_random() & 0xFFFFu) - 32768;
    return (int32_t)(((int64_t)noise * amplitude_q16) >> 15);
}

static int32_t rcs_float_to_q16(float value) {
    float scaled = value * (float)RCS_Q_ONE;
    return scaled >= 0.0f ? (int32_t)(scaled + 0.5f)
                          : (int32_t)(scaled - 0.5f);
}

static void rcs_prepare_path_cache(void)
{
    for (size_t weapon = 0; weapon < RCS_PROFILE_COUNT; ++weapon) {
        const weapon_profile_t *profile = &ALL_WEAPONS[weapon];
        int point_count = profile->length;
        if (point_count < 0) point_count = 0;
        if (point_count > RCS_MAX_PATTERN_POINTS) {
            point_count = RCS_MAX_PATTERN_POINTS;
        }

        int32_t path_x = 0;
        int32_t path_y = 0;
        for (int point = 0; point < point_count; ++point) {
            path_x += rcs_float_to_q16(profile->pattern[point].dx);
            path_y += rcs_float_to_q16(profile->pattern[point].dy);
            rcs_cached_path_x[weapon][point] = path_x;
            rcs_cached_path_y[weapon][point] = path_y;
        }
    }
    rcs_path_cache_ready = true;
}

static int32_t rcs_q16_mul(int32_t left, int32_t right) {
    int64_t product = (int64_t)left * (int64_t)right;
    int64_t rounding = ((left < 0) == (right < 0))
                     ? RCS_Q_ONE / 2 : -(RCS_Q_ONE / 2);
    product += rounding;
    return (int32_t)(product >> RCS_Q_SHIFT);
}

static int32_t rcs_div_round_i64(int64_t value, int32_t divisor) {
    int64_t magnitude = value >= 0 ? value : -value;
    int64_t rounded = (magnitude + divisor / 2) / divisor;
    return value >= 0 ? (int32_t)rounded : -(int32_t)rounded;
}

static int32_t rcs_q16_to_int_round(int32_t value) {
    if (value >= 0) {
        return (value + RCS_Q_ONE / 2) >> RCS_Q_SHIFT;
    }
    return -((-value + RCS_Q_ONE / 2) >> RCS_Q_SHIFT);
}

static uint32_t rcs_delay_to_us(float delay_ms) {
    float delay_us = delay_ms * 1000.0f;
    if (delay_us < 250.0f) return 250u;
    if (delay_us > 1000000.0f) return 1000000u;
    return (uint32_t)(delay_us + 0.5f);
}

static int32_t rcs_smoothstep_q16(int32_t t) {
    int32_t t2 = rcs_q16_mul(t, t);
    int32_t t3 = rcs_q16_mul(t2, t);
    int32_t result = 3 * t2 - 2 * t3;
    if (result < 0) return 0;
    if (result > RCS_Q_ONE) return RCS_Q_ONE;
    return result;
}

static int32_t rcs_jittered_progress_q16(int32_t progress, int32_t t) {
    if (t <= 0 || t >= RCS_Q_ONE) return progress;

    // t * (1 - t) 让扰动在片段两端自然收敛到零，避免段尾出现回弹。
    int32_t envelope = rcs_q16_mul(t, RCS_Q_ONE - t);
    int32_t noise = (int32_t)(rcs_next_random() & 0xFFFFu) - 32768;
    int32_t offset = (int32_t)(((int64_t)noise *
                                RCS_JITTER_AMPLITUDE_Q16 * envelope) >> 32);
    int32_t result = progress + offset;
    if (result < 0) return 0;
    if (result > RCS_Q_ONE) return RCS_Q_ONE;
    return result;
}

static int32_t rcs_clamp_segment_q16(int32_t value, int32_t left,
                                     int32_t right) {
    int32_t low = left < right ? left : right;
    int32_t high = left > right ? left : right;
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int32_t rcs_catmull_rom_q16(int32_t p0, int32_t p1, int32_t p2,
                                   int32_t p3, int32_t t) {
    const int32_t t2 = rcs_q16_mul(t, t);
    const int32_t t3 = rcs_q16_mul(t2, t);
    const int32_t a = 2 * p1;
    const int32_t b = -p0 + p2;
    const int32_t c = 2 * p0 - 5 * p1 + 4 * p2 - p3;
    const int32_t d = -p0 + 3 * p1 - 3 * p2 + p3;
    const int32_t value = rcs_div_round_i64(
        (int64_t)a + rcs_q16_mul(b, t) +
        rcs_q16_mul(c, t2) + rcs_q16_mul(d, t3), 2);

    // 防止急转弯处的样条过冲，保证不会生成表外的额外回弹。
    return rcs_clamp_segment_q16(value, p1, p2);
}

static void run_rcs_sequence(void) {
    const size_t profile_count = sizeof(ALL_WEAPONS) / sizeof(ALL_WEAPONS[0]);
    uint8_t weapon_id = g_current_weapon_id;
    if (weapon_id >= profile_count) weapon_id = 0;

    const weapon_profile_t* wp = &ALL_WEAPONS[weapon_id];
    int point_count = wp->length;
    if (point_count <= 0) return;
    if (point_count > RCS_MAX_PATTERN_POINTS) {
        point_count = RCS_MAX_PATTERN_POINTS;
    }
    float sens = g_game_sensitivity <= 0.01f ? 1.0f : g_game_sensitivity;
    int32_t scale_q16 = rcs_q16_mul(RCS_SENS_MULT_Q16,
                                    rcs_float_to_q16(1.0f / sens));

    // 每次新的左键按下都会重新进入本函数。把时间、武器和序号混入种子，
    // 避免相邻两次按键复用同一条曲线。
    uint32_t sequence_id = ++rcs_sequence_counter;
    rcs_rng_state ^= time_us_32() ^ (sequence_id * 0x9E3779B9u) ^
                      ((uint32_t)weapon_id * 0x85EBCA6Bu);
    if (rcs_rng_state == 0u) rcs_rng_state = 0x6D2B79F5u;

    // 这是整次压枪的变化参数，而不是每帧变化的随机数：
    // 轻微比例变化负责改变曲线形状，末端偏移负责改变中心线位置。
    int32_t curve_scale_x_q16 = RCS_Q_ONE +
                                rcs_random_signed_q16(RCS_CURVE_SCALE_VARIATION_Q16);
    int32_t curve_scale_y_q16 = RCS_Q_ONE +
                                rcs_random_signed_q16(RCS_CURVE_SCALE_VARIATION_Q16);
    int32_t curve_scale_x_sens_q16 = rcs_q16_mul(scale_q16, curve_scale_x_q16);
    int32_t curve_scale_y_sens_q16 = rcs_q16_mul(scale_q16, curve_scale_y_q16);
    int32_t curve_offset_x_q16 =
        rcs_random_signed_q16(RCS_CURVE_OFFSET_X_MAX_Q16);
    int32_t curve_offset_y_q16 =
        rcs_random_signed_q16(RCS_CURVE_OFFSET_Y_MAX_Q16);

    uint64_t begin_time = time_us_64();
    uint64_t accumulated_us = 0;
    int32_t carry_x_q16 = 0;
    int32_t carry_y_q16 = 0;
    int32_t previous_curve_offset_x_q16 = 0;
    int32_t previous_curve_offset_y_q16 = 0;

    // 表中的 dx/dy 是每发增量。先还原成累计锚点；锚点本身不移动，
    // 只在两个锚点之间用样条插值，避免平滑算法改变参考曲线的形状。
    if (!rcs_path_cache_ready) rcs_prepare_path_cache();
    const int32_t *smooth_path_x_q16 = rcs_cached_path_x[weapon_id];
    const int32_t *smooth_path_y_q16 = rcs_cached_path_y[weapon_id];

    for (int i = 0; i < point_count; i++) {
        pump_usb_tasks(0, 0, time_us_64());
        if (!(g_phys_mouse.buttons & MOUSE_BUTTON_LEFT) || !g_rcs_enabled) break;

        int32_t curve_offset_target_x_q16 = rcs_div_round_i64(
            (int64_t)curve_offset_x_q16 * (i + 1), point_count);
        int32_t curve_offset_target_y_q16 = rcs_div_round_i64(
            (int64_t)curve_offset_y_q16 * (i + 1), point_count);
        const int32_t segment_offset_x_q16 = curve_offset_target_x_q16 -
                                             previous_curve_offset_x_q16;
        const int32_t segment_offset_y_q16 = curve_offset_target_y_q16 -
                                             previous_curve_offset_y_q16;
        previous_curve_offset_x_q16 = curve_offset_target_x_q16;
        previous_curve_offset_y_q16 = curve_offset_target_y_q16;

        int32_t previous_target_x_q16 = 0;
        int32_t previous_target_y_q16 = 0;
        int subdivisions = wp->multiple > 0 ? wp->multiple : 1;
        uint32_t sub_delay_us = rcs_delay_to_us(
            (wp->pattern[i].delay / wp->sleep_divider) - wp->sleep_suber);

        for (int j = 0; j < subdivisions; j++) {
            pump_usb_tasks(0, 0, time_us_64());
            if (!(g_phys_mouse.buttons & MOUSE_BUTTON_LEFT) || !g_rcs_enabled) break;

            const int32_t t = rcs_div_round_i64(
                (int64_t)(j + 1) * RCS_Q_ONE, subdivisions);
            const int previous_index = i > 0 ? i - 1 : 0;
            const int p0_index = i > 1 ? i - 2 : previous_index;
            const int p3_index = i + 1 < point_count ? i + 1 : i;
            const int32_t path_target_x_q16 = rcs_catmull_rom_q16(
                smooth_path_x_q16[p0_index], smooth_path_x_q16[previous_index],
                smooth_path_x_q16[i], smooth_path_x_q16[p3_index], t);
            const int32_t path_target_y_q16 = rcs_catmull_rom_q16(
                smooth_path_y_q16[p0_index], smooth_path_y_q16[previous_index],
                smooth_path_y_q16[i], smooth_path_y_q16[p3_index], t);
            const int32_t path_start_x_q16 = smooth_path_x_q16[previous_index];
            const int32_t path_start_y_q16 = smooth_path_y_q16[previous_index];
            const int32_t offset_target_x_q16 = rcs_div_round_i64(
                (int64_t)segment_offset_x_q16 * (j + 1), subdivisions);
            const int32_t offset_target_y_q16 = rcs_div_round_i64(
                (int64_t)segment_offset_y_q16 * (j + 1), subdivisions);

            // 直接计算样条上的累计目标，余量均匀分布且总和严格等于锚点。
            int32_t target_x_q16 = rcs_q16_mul(
                path_target_x_q16 - path_start_x_q16, curve_scale_x_sens_q16);
            int32_t target_y_q16 = -rcs_q16_mul(
                path_target_y_q16 - path_start_y_q16, curve_scale_y_sens_q16);
            target_x_q16 += offset_target_x_q16;
            target_y_q16 += offset_target_y_q16;
            int32_t sub_dx_q16 = target_x_q16 - previous_target_x_q16;
            int32_t sub_dy_q16 = target_y_q16 - previous_target_y_q16;
            previous_target_x_q16 = target_x_q16;
            previous_target_y_q16 = target_y_q16;

            carry_x_q16 += sub_dx_q16;
            carry_y_q16 += sub_dy_q16;
            int32_t dx_int = rcs_q16_to_int_round(carry_x_q16);
            int32_t dy_int = rcs_q16_to_int_round(carry_y_q16);
            carry_x_q16 -= dx_int * RCS_Q_ONE;
            carry_y_q16 -= dy_int * RCS_Q_ONE;

            accumulated_us += sub_delay_us;
            uint64_t target_time = begin_time + accumulated_us;

            // 第一段只负责等待起始延迟，保持原有首发节奏。
            if (i == 0 && j == 0) {
                pump_usb_tasks(0, 0, target_time);
                continue;
            }

            uint64_t start_time = time_us_64();
            uint64_t duration_us = target_time > start_time
                                 ? target_time - start_time : 0;
            if (duration_us <= 1500u || (dx_int == 0 && dy_int == 0)) {
                pump_usb_tasks((int16_t)dx_int, (int16_t)dy_int, target_time);
                continue;
            }

            int steps = (int)((duration_us + 500u) / 1000u);
            if (steps < 2) steps = 2;
            if (steps > 20) steps = 20;

            int32_t moved_x = 0;
            int32_t moved_y = 0;
            int32_t previous_progress = 0;
            for (int s = 1; s <= steps; s++) {
                int32_t t = rcs_div_round_i64((int64_t)s * RCS_Q_ONE, steps);
                int32_t progress = rcs_smoothstep_q16(t);
                progress = rcs_jittered_progress_q16(progress, t);
                if (progress < previous_progress) progress = previous_progress;
                if (s == steps) progress = RCS_Q_ONE;
                int32_t current_x = rcs_q16_mul(dx_int, progress);
                int32_t current_y = rcs_q16_mul(dy_int, progress);
                int32_t step_dx = current_x - moved_x;
                int32_t step_dy = current_y - moved_y;
                uint64_t step_target = start_time +
                    (uint64_t)rcs_div_round_i64((int64_t)duration_us * s, steps);

                pump_usb_tasks((int16_t)step_dx, (int16_t)step_dy, step_target);
                moved_x = current_x;
                moved_y = current_y;
                previous_progress = progress;
            }

            // 最后一次修正保证整数输出与目标位移完全一致。
            if (moved_x != dx_int || moved_y != dy_int) {
                pump_usb_tasks((int16_t)(dx_int - moved_x),
                               (int16_t)(dy_int - moved_y), target_time);
            } else {
                pump_usb_tasks(0, 0, target_time);
            }
        }
    }

    while ((g_phys_mouse.buttons & MOUSE_BUTTON_LEFT) && g_rcs_enabled) {
        pump_usb_tasks(0, 0, time_us_64() + 1000);
    }
}

// =====================================================================

static bool resolve_host_target(uint8_t target, uint8_t *dev_addr, uint8_t *instance)
{
  if (dev_addr == NULL || instance == NULL) return false;

  if (target == 0) {
    if (!g_mouse_host_valid) return false;
    *dev_addr = g_mouse_host_dev_addr;
    *instance = g_mouse_host_instance;
    return true;
  }
  if (target == 1) {
    if (!g_vendor_host_valid) return false;
    *dev_addr = g_vendor_host_dev_addr;
    *instance = g_vendor_host_instance;
    return true;
  }
  if (!g_feature_host_valid) return false;
  *dev_addr = g_feature_host_dev_addr;
  *instance = g_feature_host_instance;
  return true;
}

static void bridge_host_task(void) {
  if (g_get_report_bridge.pending && !g_get_report_bridge.busy) {
    uint8_t host_dev = 0;
    uint8_t host_instance = 0;
    if (resolve_host_target(g_get_report_bridge.target, &host_dev, &host_instance)) {
      if (tuh_hid_get_report(host_dev, host_instance,
                             g_get_report_bridge.report_id,
                             g_get_report_bridge.report_type,
                             g_get_report_bridge.data,
                             g_get_report_bridge.request_len)) {
        g_get_report_bridge.pending = false;
        g_get_report_bridge.busy = true;
      }
    }
    return;
  }

  if (g_bridge_control_busy || queue_is_empty(&bridge_output_queue) ||
      !queue_try_peek(&bridge_output_queue, &g_bridge_inflight)) {
    return;
  }

  uint8_t host_dev = 0;
  uint8_t host_instance = 0;

  if (resolve_host_target(g_bridge_inflight.target, &host_dev, &host_instance)) {
    bool sent = false;
    if (g_bridge_inflight.interrupt_out) {
      // Preserve the physical MI_03 transport: Report ID 08 followed by a
      // 63-byte payload on interrupt endpoint 0x04.
      sent = tuh_hid_send_report(host_dev, host_instance,
                                 g_bridge_inflight.report_id,
                                 g_bridge_inflight.data,
                                 g_bridge_inflight.len);
    } else {
      // Feature reports and control-originated Output reports use SET_REPORT.
      sent = tuh_hid_set_report(host_dev, host_instance,
                                g_bridge_inflight.report_id,
                                g_bridge_inflight.report_type,
                                g_bridge_inflight.data,
                                g_bridge_inflight.len);
    }
    if (sent) g_bridge_control_busy = true;
  }
}

void core1_entry(void) {
  tusb_rhport_init_t host_init = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL};
  // The physical mouse uses its 79-byte report descriptor: buttons + two
  // 16-bit relative axes + wheel + pan. TinyUSB defaults HID interfaces to
  // Boot Protocol unless Report Protocol is selected before host init.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
  tusb_init(BOARD_TUH_RHPORT, &host_init);

  while (1) {
    tuh_task(); 
    bridge_host_task();
    led_blinking_task();
  }
}

/*------------- MAIN -------------*/
int main(void) {
  board_init();

  queue_init(&keyboard_report_queue, sizeof(keyboard_report_t), KEYBOARD_QUEUE_SIZE);
  queue_init(&mouse_report_queue, sizeof(hid_mouse_state_t), MOUSE_QUEUE_SIZE);
  queue_init(&advanced_report_queue, sizeof(advanced_report_t), ADVANCED_QUEUE_SIZE); 
  queue_init(&vendor_input_queue, sizeof(bridge_report_t), VENDOR_QUEUE_SIZE);
  queue_init(&bridge_output_queue, sizeof(bridge_report_t), VENDOR_QUEUE_SIZE);
  rcs_prepare_path_cache();

  tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

  multicore_launch_core1(core1_entry);

  while (1) {
    pump_usb_tasks(0, 0, time_us_64());

    if (g_rcs_enabled && (g_phys_mouse.buttons & MOUSE_BUTTON_LEFT)) {
        run_rcs_sequence();
    }

  }

  return 0;
}

// --------------------------------------------------------------------+
// USB HID Device Callbacks 
// --------------------------------------------------------------------+

void tud_mount_cb(void) {
  g_usb_suspended = false;
  g_remote_wakeup_allowed = false;
  blink_interval_ms = BLINK_MOUNTED;
}

void tud_umount_cb(void) {
  g_usb_suspended = false;
  g_remote_wakeup_allowed = false;
  blink_interval_ms = BLINK_NOT_MOUNTED;
}

void tud_suspend_cb(bool remote_wakeup_en) {
  g_usb_suspended = true;
  g_remote_wakeup_allowed = remote_wakeup_en;
  blink_interval_ms = BLINK_SUSPENDED;
}

void tud_resume_cb(void) {
  g_usb_suspended = false;
  g_remote_wakeup_allowed = false;
  blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

static uint16_t copy_report(uint8_t *buffer, uint16_t reqlen,
                            uint8_t const *source, uint16_t source_len) {
  uint16_t length = reqlen < source_len ? reqlen : source_len;
  memcpy(buffer, source, length);
  return length;
}

// Local control reports share the physical MI_03 Report ID 0x08. The
// two-byte marker keeps them distinct from raw vendor traffic being proxied.
static bool is_local_control_report(uint8_t report_id,
                                     hid_report_type_t report_type,
                                     uint8_t const *buffer,
                                     uint16_t bufsize) {
  return report_id == 0x08u &&
         report_type == HID_REPORT_TYPE_OUTPUT &&
         buffer != NULL && bufsize >= 3u &&
         buffer[0] == LOCAL_CMD_MAGIC0 &&
         buffer[1] == LOCAL_CMD_MAGIC1 &&
         buffer[2] == LOCAL_CMD_MACRO;
}

static uint16_t passthrough_get_report(uint8_t target, uint8_t report_id,
                                       hid_report_type_t report_type,
                                       uint8_t *buffer, uint16_t reqlen)
{
  if (buffer == NULL || reqlen == 0 || g_get_report_bridge.pending ||
      g_get_report_bridge.busy) {
    return 0;
  }

  uint8_t host_dev = 0;
  uint8_t host_instance = 0;
  if (!resolve_host_target(target, &host_dev, &host_instance)) {
    return 0;
  }
  (void)host_dev;
  (void)host_instance;

  uint16_t request_len = reqlen;
  if (report_id != 0 && request_len < BRIDGE_REPORT_MAX_LEN) {
    request_len++;
  }
  if (request_len > BRIDGE_REPORT_MAX_LEN) {
    request_len = BRIDGE_REPORT_MAX_LEN;
  }

  g_get_report_bridge.target = target;
  g_get_report_bridge.report_id = report_id;
  g_get_report_bridge.report_type = (uint8_t)report_type;
  g_get_report_bridge.request_len = request_len;
  g_get_report_bridge.actual_len = 0;
  g_get_report_bridge.done = false;
  g_get_report_bridge.pending = true;
  __dmb();

  uint32_t start_us = time_us_32();
  while (!g_get_report_bridge.done &&
         (uint32_t)(time_us_32() - start_us) < 30000u) {
    sleep_us(50);
  }

  if (!g_get_report_bridge.done) {
    return 0;
  }

  uint16_t actual_len = g_get_report_bridge.actual_len;
  if (actual_len > BRIDGE_REPORT_MAX_LEN) {
    actual_len = BRIDGE_REPORT_MAX_LEN;
  }

  uint16_t source_offset = 0;
  if (report_id != 0 && actual_len > 0 &&
      g_get_report_bridge.data[0] == report_id) {
    source_offset = 1;
  }
  if (actual_len <= source_offset) {
    g_get_report_bridge.done = false;
    return 0;
  }

  uint16_t available = actual_len - source_offset;
  uint16_t copy_length = available < reqlen ? available : reqlen;
  memcpy(buffer, &g_get_report_bridge.data[source_offset], copy_length);
  g_get_report_bridge.done = false;
  return copy_length;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
  if (buffer == NULL || reqlen == 0) return 0;

  if (instance == ITF_NUM_ADVANCED &&
      (report_type == HID_REPORT_TYPE_FEATURE ||
       report_type == HID_REPORT_TYPE_INPUT)) {
    uint16_t length = passthrough_get_report(2, report_id, report_type,
                                             buffer, reqlen);
    if (length != 0) return length;
  }

  if (instance == ITF_NUM_VENDOR &&
      (report_type == HID_REPORT_TYPE_FEATURE ||
       report_type == HID_REPORT_TYPE_INPUT)) {
    uint16_t length = passthrough_get_report(1, report_id, report_type,
                                             buffer, reqlen);
    if (length != 0) return length;
  }

  if (instance == ITF_NUM_KEYBOARD && report_type == HID_REPORT_TYPE_OUTPUT &&
      report_id == 0) {
    buffer[0] = g_keyboard_leds;
    return 1;
  }

  if (instance == ITF_NUM_MOUSE && report_type == HID_REPORT_TYPE_INPUT &&
      report_id == ATK_MOUSE_REPORT_ID) {
    atk_mouse_report_t current = {
        .buttons = g_phys_mouse.buttons,
        .x = g_phys_mouse.x,
        .y = g_phys_mouse.y,
        .wheel = g_phys_mouse.wheel
    };
    return copy_report(buffer, reqlen, (uint8_t const *)&current,
                       sizeof(current));
  }

  if (instance == ITF_NUM_ADVANCED) {
    if (report_type == HID_REPORT_TYPE_FEATURE && report_id == 0x03) {
      return copy_report(buffer, reqlen, g_feature_report_03,
                         sizeof(g_feature_report_03));
    }
    if (report_type == HID_REPORT_TYPE_FEATURE && report_id == 0x04) {
      return copy_report(buffer, reqlen, g_feature_report_04,
                         sizeof(g_feature_report_04));
    }
    if (report_type == HID_REPORT_TYPE_INPUT && report_id == 0x01) {
      uint8_t empty_report[2] = {0};
      return copy_report(buffer, reqlen, empty_report, sizeof(empty_report));
    }
    if (report_type == HID_REPORT_TYPE_INPUT && report_id == 0x02) {
      uint8_t empty_report[1] = {0};
      return copy_report(buffer, reqlen, empty_report, sizeof(empty_report));
    }
    if (report_type == HID_REPORT_TYPE_INPUT && report_id == 0x05) {
      uint8_t empty_report[15] = {0};
      return copy_report(buffer, reqlen, empty_report, sizeof(empty_report));
    }
  }

  if (instance == ITF_NUM_VENDOR && report_type == HID_REPORT_TYPE_INPUT) {
    if (report_id == g_vendor_last_input_id && g_vendor_last_input_len != 0) {
      return copy_report(buffer, reqlen, g_vendor_last_input,
                         g_vendor_last_input_len);
    }
    if (report_id == 0x08) {
      return copy_report(buffer, reqlen, g_vendor_report_08,
                         sizeof(g_vendor_report_08));
    }
    if (report_id == 0x09) {
      return copy_report(buffer, reqlen, g_vendor_report_09,
                         sizeof(g_vendor_report_09));
    }
    if (report_id == 0x0A) {
      return copy_report(buffer, reqlen, g_vendor_report_0A,
                         sizeof(g_vendor_report_0A));
    }
  }

  return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
  if (buffer == NULL) return;

  if (itf == ITF_NUM_KEYBOARD && report_type == HID_REPORT_TYPE_OUTPUT &&
      report_id == 0 && bufsize >= 1) {
    g_keyboard_leds = buffer[0];
    return;
  }

  if (itf == ITF_NUM_ADVANCED && report_type == HID_REPORT_TYPE_FEATURE &&
      (report_id == 0x03 || report_id == 0x04)) {
    uint8_t *destination = report_id == 0x03
        ? g_feature_report_03 : g_feature_report_04;
    uint16_t length = bufsize < ATK_FEATURE_REPORT_LEN
        ? bufsize : ATK_FEATURE_REPORT_LEN;
    memset(destination, 0, ATK_FEATURE_REPORT_LEN);
    memcpy(destination, buffer, length);

    bridge_report_t forward = {0};
    forward.target = 2;
    forward.report_id = report_id;
    forward.report_type = (uint8_t)report_type;
    if (report_id != 0) {
      uint16_t copy_length = length > BRIDGE_REPORT_MAX_LEN
          ? BRIDGE_REPORT_MAX_LEN : length;
      memcpy(forward.data, buffer, copy_length);
      forward.len = (uint8_t)copy_length;
    } else {
      forward.len = (uint8_t)length;
      memcpy(forward.data, buffer, length);
    }
    queue_try_add(&bridge_output_queue, &forward);
    return;
  }

  if ((report_type == HID_REPORT_TYPE_OUTPUT ||
       report_type == HID_REPORT_TYPE_FEATURE) &&
      itf == ITF_NUM_VENDOR) {
    if (!is_local_control_report(report_id, report_type, buffer, bufsize)) {
    bridge_report_t forward = {0};
    forward.target = (uint8_t)(itf == ITF_NUM_MOUSE ? 0u : 1u);
    forward.report_id = report_id;
    forward.report_type = (uint8_t)report_type;

    uint8_t const *payload = buffer;
    uint16_t payload_len = bufsize;

    // TinyUSB reports interrupt OUT data as report_id=0 to this callback.
    // MI_03 puts Report ID 08 in the first byte of the endpoint packet.
    if (itf == ITF_NUM_VENDOR && report_type == HID_REPORT_TYPE_OUTPUT &&
        report_id == 0 && bufsize >= 1u && buffer[0] == 0x08u) {
      forward.report_id = 0x08u;
      forward.interrupt_out = 1u;
      payload = &buffer[1];
      payload_len = (uint16_t)(bufsize - 1u);
    }

    if (forward.report_id != 0) {
      uint16_t copy_length = payload_len > BRIDGE_REPORT_MAX_LEN
          ? BRIDGE_REPORT_MAX_LEN : payload_len;
      memcpy(forward.data, payload, copy_length);
      forward.len = (uint8_t)copy_length;
    } else {
      forward.len = payload_len > BRIDGE_REPORT_MAX_LEN
          ? BRIDGE_REPORT_MAX_LEN : (uint8_t)payload_len;
      memcpy(forward.data, payload, forward.len);
    }
    queue_try_add(&bridge_output_queue, &forward);
    return;
    }
  }

  if (report_type != HID_REPORT_TYPE_OUTPUT) return;

  // ★ 新增：0x0A 宏指令 / Aimbot 移动 / 0x0C 上位机配置 接收逻辑
  // buffer[0] = command type:
  //   1 = 左键点击, 4 = Mouse4, 5 = Mouse5
  //   0x0B = Aimbot 移动 (buffer[1-2]=dx, buffer[3-4]=dy)
  //   0x0C = 上位机配置 (buffer[4]=武器ID, buffer[5..8]=灵敏度float, buffer[9]=开关)
  if (itf == ITF_NUM_VENDOR &&
      is_local_control_report(report_id, report_type, buffer, bufsize) &&
      buffer[2] == LOCAL_CMD_MACRO && bufsize >= 4u) {
    uint8_t cmd = buffer[3];

    if (cmd == 0x0B && bufsize >= 8u) {
      // Aimbot 鼠标移动
      int16_t aim_dx = 0, aim_dy = 0;
      memcpy(&aim_dx, &buffer[4], sizeof(int16_t));
      memcpy(&aim_dy, &buffer[6], sizeof(int16_t));
      accum_dx += aim_dx;
      accum_dy += aim_dy;
    }
    else if (cmd == LOCAL_CMD_CONFIG && bufsize >= 10u) {
      // 复用 Vendor Report 0x08 的本地控制帧，未改动任何报告描述符。
      // 只更新 RCS 三个全局参数，完全不经过鼠标队列，鼠标通路保持 1:1 透传。
      const uint8_t weapon_count = (uint8_t)(sizeof(ALL_WEAPONS) /
                                             sizeof(ALL_WEAPONS[0]));
      uint8_t weapon_id = buffer[4];
      if (weapon_id >= weapon_count) weapon_id = 0;

      float sens = 0.75f;
      memcpy(&sens, &buffer[5], sizeof(float));
      if (sens < 0.01f || sens > 20.0f) sens = 0.75f;

      g_current_weapon_id = weapon_id;
      g_game_sensitivity = sens;
      g_rcs_enabled = (buffer[9] != 0);
    }
    else {
      // 宏指令（按键点击）
      hid_mouse_state_t rpt = { .buttons = g_phys_mouse.buttons, .x = 0, .y = 0, .wheel = 0 };

      if (cmd == 1) {
          rpt.buttons |= MOUSE_BUTTON_LEFT;
          queue_mouse_report(&rpt);
          rpt.buttons &= (uint8_t)(~MOUSE_BUTTON_LEFT);
          queue_mouse_report(&rpt);
      } else if (cmd == 4) {
          rpt.buttons |= MOUSE_BUTTON_4;
          queue_mouse_report(&rpt);
          rpt.buttons &= (uint8_t)(~MOUSE_BUTTON_4);
          queue_mouse_report(&rpt);
      } else if (cmd == 5) {
          rpt.buttons |= MOUSE_BUTTON_5;
          queue_mouse_report(&rpt);
          rpt.buttons &= (uint8_t)(~MOUSE_BUTTON_5);
          queue_mouse_report(&rpt);
      }
    }
  }
} // 之前报错是因为漏掉了这个花括号

void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                    uint8_t report_id, uint8_t report_type,
                                    uint16_t len) {
  (void)dev_addr;
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)len;

  if (g_bridge_control_busy) {
    bridge_report_t completed;
    (void)queue_try_remove(&bridge_output_queue, &completed);
    g_bridge_control_busy = false;
  }
}

void tuh_hid_report_sent_cb(uint8_t dev_addr, uint8_t instance,
                            uint8_t const *report, uint16_t len) {
  (void)dev_addr;
  (void)instance;
  (void)report;
  (void)len;

  // Completion callback for physical interrupt OUT transfers.
  if (g_bridge_control_busy) {
    bridge_report_t completed;
    (void)queue_try_remove(&bridge_output_queue, &completed);
    g_bridge_control_busy = false;
  }
}

void tuh_hid_get_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                    uint8_t report_id, uint8_t report_type,
                                    uint16_t len) {
  (void)dev_addr;
  (void)instance;
  (void)report_id;
  (void)report_type;
  g_get_report_bridge.actual_len = len;
  __dmb();
  g_get_report_bridge.busy = false;
  g_get_report_bridge.done = (len != 0);
}

// --------------------------------------------------------------------+
// Host HID Callbacks (读取物理外接设备)
// --------------------------------------------------------------------+

void tuh_umount_cb(uint8_t dev_addr) {
  bool any = false;
  for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX; addr++) {
    if (addr == dev_addr) continue; 
    if (tuh_mounted(addr)) { any = true; break; }
  }
  if (!any) blink_interval_ms = BLINK_NOT_MOUNTED;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  if (instance >= CFG_TUH_HID) return;

  blink_interval_ms = BLINK_MOUNTED;
  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);
  hid_devices[instance].dev_addr = dev_addr;
  hid_devices[instance].idx = instance;
  hid_devices[instance].protocol = proto;
  hid_devices[instance].mouse_report_id =
      descriptor_first_report_id(desc_report, desc_len);
  hid_devices[instance].mouse_16bit_axes =
      descriptor_has_16bit_relative_axes(desc_report, desc_len);
  hid_devices[instance].mouse_interface =
      proto == HID_ITF_PROTOCOL_MOUSE ||
      descriptor_has_mouse_usage(desc_report, desc_len);
  hid_devices[instance].vendor_interface =
      descriptor_has_vendor_64(desc_report, desc_len);
  hid_devices[instance].feature_interface =
      descriptor_has_feature_reports(desc_report, desc_len);
  hid_devices[instance].vendor_report_id =
      hid_devices[instance].vendor_interface
          ? descriptor_first_report_id(desc_report, desc_len) : 0;

  if (hid_devices[instance].mouse_interface) {
    g_mouse_host_valid = true;
    g_mouse_host_dev_addr = dev_addr;
    g_mouse_host_instance = instance;
  }
  if (hid_devices[instance].vendor_interface) {
    g_vendor_host_valid = true;
    g_vendor_host_dev_addr = dev_addr;
    g_vendor_host_instance = instance;
  }
  if (hid_devices[instance].feature_interface) {
    g_feature_host_valid = true;
    g_feature_host_dev_addr = dev_addr;
    g_feature_host_instance = instance;
  }

  if (proto == HID_ITF_PROTOCOL_KEYBOARD ||
      proto == HID_ITF_PROTOCOL_MOUSE ||
      proto == HID_ITF_PROTOCOL_NONE) {
    tuh_hid_receive_report(dev_addr, instance);
  }
}

void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t instance, uint8_t protocol) {
  (void)dev_addr;
  hid_devices[instance].protocol = protocol;
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  if (g_mouse_host_valid && g_mouse_host_dev_addr == dev_addr &&
      g_mouse_host_instance == instance) {
    g_mouse_host_valid = false;
  }
  if (g_vendor_host_valid && g_vendor_host_dev_addr == dev_addr &&
      g_vendor_host_instance == instance) {
    g_vendor_host_valid = false;
    g_vendor_last_input_len = 0;
  }
  if (g_feature_host_valid && g_feature_host_dev_addr == dev_addr &&
      g_feature_host_instance == instance) {
    g_feature_host_valid = false;
  }
  if (instance < CFG_TUH_HID) {
    memset(&hid_devices[instance], 0, sizeof(hid_devices[instance]));
  }
  control_previous_buttons = 0;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  if (len == 0) {
    bool any = false;
    for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX; addr++) {
      if (addr == dev_addr) continue; 
      if (tuh_mounted(addr)) { any = true; break; }
    }
    if (any) {
      board_delay(1000);
      tuh_hid_receive_report(dev_addr, instance);
    }
    return;
  }

  uint8_t proto = tuh_hid_interface_protocol(dev_addr, instance);

  // Mouse reports use the same aggregation path as the known-good backup.
  // The parser accepts both the 6/7-byte payload and an optional ID prefix.
  if (instance < CFG_TUH_HID && hid_devices[instance].mouse_interface) {
    hid_mouse_state_t parsed_mouse;
    if (parse_mouse_report(instance, report, len, &parsed_mouse)) {
      queue_mouse_report(&parsed_mouse);
    }
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }

  if (instance < CFG_TUH_HID && hid_devices[instance].vendor_interface) {
    uint8_t offset = hid_devices[instance].vendor_report_id != 0 ? 1u : 0u;
    if (len > offset) {
      bridge_report_t vendor = {0};
      vendor.target = 1;
      vendor.report_id = hid_devices[instance].vendor_report_id;
      vendor.len = (uint8_t)((len - offset > BRIDGE_REPORT_MAX_LEN)
          ? BRIDGE_REPORT_MAX_LEN : (len - offset));
      memcpy(vendor.data, report + offset, vendor.len);
      memcpy(g_vendor_last_input, vendor.data, vendor.len);
      g_vendor_last_input_len = vendor.len;
      g_vendor_last_input_id = vendor.report_id;
      queue_try_add(&vendor_input_queue, &vendor);
    }
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }

  if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
    if (len < 8) { tuh_hid_receive_report(dev_addr, instance); return; }
    keyboard_report_t kbd_report;
    kbd_report.modifier = report[0]; 
    for (int i = 0; i < 6; i++) { kbd_report.keycode[i] = report[2 + i]; }
    queue_try_add(&keyboard_report_queue, &kbd_report);
  }
  else if (proto == HID_ITF_PROTOCOL_MOUSE) {
    hid_mouse_state_t parsed_mouse;
    if (parse_mouse_report(instance, report, len, &parsed_mouse)) {
      queue_mouse_report(&parsed_mouse);
      tuh_hid_receive_report(dev_addr, instance);
      return;
    }
#if 0
    
    // ★ 恢复原版最稳定的 Report ID 偏移检测，绝不能动态检测按键！
    uint8_t offset = 0;
    // 只有包长严格匹配，并且首字节符合 Report ID 范围时才偏移
    if ((len == 6 || len == 8) && report[0] > 0 && report[0] <= 0x0F) {
        offset = 1;
    }

    int data_len = len - offset;
    if (data_len >= 3) {
        // 读取基本主按键和 8位 坐标
        uint8_t buttons = report[offset];
        int16_t x = (int8_t)report[offset + 1];
        int16_t y = (int8_t)report[offset + 2];
        
        int8_t wheel = 0;

        // ★ 核心修复：精准定位长包鼠标的滚轮和侧键
        if (data_len >= 5) {
            uint8_t extra_btn = report[offset + 3]; // 第 4 字节往往是附加的按键（藏着上侧键）
            wheel = (int8_t)report[offset + 4];     // 第 5 字节才是真正的纵向滚轮
            
            if (data_len >= 6) {
            }

            // 找回失灵的“上侧键” (前进键)
            // 如果常规按键的最高位没扫到上侧键，我们去 extra_btn 里面把它揪出来
            if (!(buttons & MOUSE_BUTTON_5)) {
                // 常见的电竞鼠标会把侧键放在 extra_btn 的第0、1位，或者第4、5位
                if (extra_btn & 0x01 || extra_btn & 0x10) buttons |= MOUSE_BUTTON_4;
                if (extra_btn & 0x02 || extra_btn & 0x20) buttons |= MOUSE_BUTTON_5;
            }
        } else if (data_len == 4) {
            // 兼容普通的 4 字节标准鼠标
            wheel = (int8_t)report[offset + 3];
        }

        hid_mouse_state_t rpt = { .buttons = buttons, .x = x, .y = y, .wheel = wheel };
        queue_try_add(&mouse_report_queue, &rpt);
    }
  }
#endif
  }
  else if (proto == HID_ITF_PROTOCOL_NONE) {
    if (len > 0) {
        advanced_report_t adv;
        adv.report_id = report[0];
        
        // 补充 (uint8_t) 强转以消除长度计算时的隐式 int 截断警告
        adv.len = (uint8_t)((len - 1 > 16) ? 16 : (len - 1));
        
        memcpy(adv.data, report + 1, adv.len);
        queue_try_add(&advanced_report_queue, &adv);
    }
  }

  tuh_hid_receive_report(dev_addr, instance);
}

void led_blinking_task(void) {
  static uint32_t start_ms = 0;
  static bool led_state = false;
  uint32_t interval = blink_interval_ms;

  if (board_millis() - start_ms < interval) return; 
  start_ms += interval;
  board_led_write(led_state);
  led_state = 1 - led_state; 
}
