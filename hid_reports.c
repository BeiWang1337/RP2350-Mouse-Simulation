#include "hid_reports.h"
#include "usb_descriptors.h"
#include <string.h>

bool send_keyboard_report(uint8_t modifier, const uint8_t keycode[6]) {
  // ATK 键盘描述符长度为 8 字节，并且无 Report ID 前缀 [cite: 66-69]
  uint8_t buffer[8] = {0};
  buffer[0] = modifier;
  buffer[1] = 0; // Reserved
  memcpy(&buffer[2], keycode, 6);
  return tud_hid_n_report(ITF_NUM_KEYBOARD, 0, buffer, 8);
}

bool send_mouse_report(uint8_t buttons, int16_t x, int16_t y,
                       int8_t vertical, int8_t horizontal) {
  (void)horizontal;

  atk_mouse_report_t report = {
      .buttons = buttons,
      .x = x,
      .y = y,
      .wheel = vertical
  };
  // 按照 ATK 格式将 7 字节推送到接口 0 [cite: 87]
  return tud_hid_n_report(ITF_NUM_MOUSE, ATK_MOUSE_REPORT_ID,
                          &report, sizeof(report));
}

static hid_mouse_state_t g_mouse_state = {0};

bool mouse_press(uint8_t button) {
  g_mouse_state.buttons |= button;
  return send_mouse_report(g_mouse_state.buttons, 0, 0, 0, 0);
}

bool mouse_release(uint8_t button) {
  g_mouse_state.buttons &= ~button;
  return send_mouse_report(g_mouse_state.buttons, 0, 0, 0, 0);
}

bool mouse_move(int16_t x, int16_t y) {
  bool ret = send_mouse_report(g_mouse_state.buttons, x, y, 0, 0);
  return ret;
}

bool mouse_scroll(int8_t vertical, int8_t horizontal) {
  bool ret = send_mouse_report(g_mouse_state.buttons, 0, 0,
                               vertical, horizontal);
  return ret;
}
