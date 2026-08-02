#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 BLE GATT 串行服务，手机写入指令字符即控制小车 */
void ble_car_init(void);

/* BLE 是否已连接（调试用） */
bool ble_car_is_connected(void);

#ifdef __cplusplus
}
#endif
