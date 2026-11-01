#ifndef _CST836U_TOUCH_H_
#define _CST836U_TOUCH_H_

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// CST836U 单点触摸（I2C 0x15，0x00 起 11 字节）
// 使用独立的触摸 I2C 总线，必须传入已创建的 bus handle

bool cst836u_touch_init(i2c_master_bus_handle_t bus);
bool cst836u_touch_read(int *x, int *y);

#ifdef __cplusplus
}
#endif

#endif  // _CST836U_TOUCH_H_
