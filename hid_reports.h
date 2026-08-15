#ifndef HID_REPORTS_H_
#define HID_REPORTS_H_

#include "tusb.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 匹配 ATK 描述符的 7 字节鼠标包 [cite: 30]
typedef struct __attribute__((packed))
{
  uint8_t buttons;
  int16_t x;       // ATK 采用 16位 坐标系！[cite: 81]
  int16_t y;
  int8_t wheel;
} atk_mouse_report_t;

#define ATK_MOUSE_REPORT_LEN ((uint16_t)sizeof(atk_mouse_report_t))
#define ATK_MOUSE_REPORT_ID 0x02u

_Static_assert(sizeof(atk_mouse_report_t) == 6,
               "mouse payload length mismatch");


// API
bool send_keyboard_report(uint8_t modifier, const uint8_t keycode[6]);
bool send_mouse_report(uint8_t buttons, int16_t x, int16_t y,
                       int8_t vertical, int8_t horizontal);

#define MOUSE_BUTTON_LEFT (1 << 0)
#define MOUSE_BUTTON_RIGHT (1 << 1)
#define MOUSE_BUTTON_MIDDLE (1 << 2)
#define MOUSE_BUTTON_4 (1 << 3)
#define MOUSE_BUTTON_5 (1 << 4)

typedef struct
{
  uint8_t buttons;
  int16_t x;       // 状态机同步变更为 16 位
  int16_t y;
  int8_t wheel;
} hid_mouse_state_t;

bool mouse_press(uint8_t button);
bool mouse_release(uint8_t button);
bool mouse_click(uint8_t button);
bool mouse_move(int16_t x, int16_t y);
bool mouse_scroll(int8_t vertical, int8_t horizontal);

#ifdef __cplusplus
}
#endif

#endif // HID_REPORTS_H_
