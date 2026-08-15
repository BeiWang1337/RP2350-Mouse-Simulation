#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

// 1:1 对齐 ATK 抓包的 4 个接口顺序
enum {
  ITF_NUM_KEYBOARD = 0, // Interface 0: 键盘
  ITF_NUM_MOUSE = 1,    // Interface 1: 鼠标
  ITF_NUM_ADVANCED = 2, // Interface 2: 消费控制/高级扩展
  ITF_NUM_VENDOR = 3,   // Interface 3: 厂商自定义驱动通信
  ITF_NUM_TOTAL
};

#endif /* USB_DESCRIPTORS_H_ */
