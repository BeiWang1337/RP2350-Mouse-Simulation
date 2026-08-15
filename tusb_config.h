#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define USE_CDC 0 
#define INTERVAL_MS 1 

// ================== ATK 1:1 完美伪装参数 ==================
#define USB_VID 0x373B 
#define USB_PID 0x1155 
#define USB_MANUFACTURER "ATK" 
#define USB_PRODUCT "Wireless mouse 8k dongle-L1" 
#define USB_SERIAL_NUMBER "Wireless mouse 8k dongle-L1" 
#define USB_MAX_POWER_MA 100 
#define USB_FIRMWARE_VERSION 0x0101 
// ==========================================================

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 1
#endif
#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED BOARD_TUD_MAX_SPEED
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED BOARD_TUH_MAX_SPEED

#if CFG_TUSB_MCU == OPT_MCU_RP2040 || CFG_TUSB_MCU == OPT_MCU_RP2350
#define CFG_TUH_RPI_PIO_USB 1
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64 

#define CFG_TUD_MSC 0

#define CFG_TUD_HID 4 // ATK具备4个接口
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_CDC 0

// Vendor 接口 0x04 输出端点最大包长 64 bytes
#define CFG_TUD_HID_EP_BUFSIZE 64

#define USE_ADAFRUIT_FEATHER_RP2040_USBHOST 1
#define PICO_DEFAULT_PIO_USB_VBUSEN_PIN 18
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 1
#define CFG_TUH_DEVICE_MAX (CFG_TUH_HUB ? 4 : 1)
#define CFG_TUH_HID (4 * CFG_TUH_DEVICE_MAX) 
#define CFG_TUH_HID_EPIN_BUFSIZE 256
#define CFG_TUH_HID_EPOUT_BUFSIZE 256

#ifdef __cplusplus
}
#endif
#endif
