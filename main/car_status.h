#pragma once
#include <stddef.h>
#include <stdint.h>

#define CAR_SCAN_MAX      20
#define CAR_WIFI_LOG_MAX  16

typedef struct {
    char   ssid[33];
    int8_t rssi;
    uint8_t chan;
    uint8_t auth;    /* wifi_auth_mode_t 数值 */
    uint8_t target;  /* 1 = 匹配目标 SSID */
} scan_ap_t;

/* 生成小车实时状态 JSON（Web /status 与 BLE 状态特征共用） */
void car_status_json(char *buf, size_t len);

/* WiFi 日志 / 扫描结果扩展（供网页展示） */
void car_status_set_wifi_state(const char *s);                       /* off/scanning/connecting/connected/disconnected */
void car_status_set_scan_count(uint16_t n);
void car_status_set_scan_ap(uint16_t i, const char *ssid, int8_t rssi,
                            uint8_t chan, uint8_t auth, uint8_t target);
void car_status_wifi_log(const char *msg);                           /* 追加一WiFi 事件日志 */

/* BLE 广播状态：0=广播中，>0=ble_gap_adv_start 错误码，-1=未启动 */
void car_status_set_ble_adv(int state);
