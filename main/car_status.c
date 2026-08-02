#include "car_status.h"
#include "motor.h"
#include "ble_car.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static char     g_wifi_state[16] = "off";
static scan_ap_t g_scan[CAR_SCAN_MAX];
static uint16_t g_scan_n = 0;
static char     g_wifi_log[CAR_WIFI_LOG_MAX][96];
static uint8_t  g_wifi_log_head = 0;
static uint8_t  g_wifi_log_cnt  = 0;
static int      g_ble_adv = -1;

/* 保护所有 g_* 全局状态：event loop 任务、reconnect 任务、web server 任务并发读写 */
static portMUX_TYPE s_cs_mux = portMUX_INITIALIZER_UNLOCKED;

void car_status_set_wifi_state(const char *s)
{
    portENTER_CRITICAL(&s_cs_mux);
    strncpy(g_wifi_state, s ? s : "off", sizeof(g_wifi_state) - 1);
    g_wifi_state[sizeof(g_wifi_state) - 1] = 0;
    portEXIT_CRITICAL(&s_cs_mux);
}

void car_status_set_scan_count(uint16_t n)
{
    portENTER_CRITICAL(&s_cs_mux);
    g_scan_n = (n > CAR_SCAN_MAX) ? CAR_SCAN_MAX : n;
    portEXIT_CRITICAL(&s_cs_mux);
}

void car_status_set_scan_ap(uint16_t i, const char *ssid, int8_t rssi,
                            uint8_t chan, uint8_t auth, uint8_t target)
{
    if (i >= CAR_SCAN_MAX) return;
    portENTER_CRITICAL(&s_cs_mux);
    strncpy(g_scan[i].ssid, ssid ? ssid : "", sizeof(g_scan[i].ssid) - 1);
    g_scan[i].ssid[sizeof(g_scan[i].ssid) - 1] = 0;
    g_scan[i].rssi   = rssi;
    g_scan[i].chan   = chan;
    g_scan[i].auth   = auth;
    g_scan[i].target = target ? 1 : 0;
    portEXIT_CRITICAL(&s_cs_mux);
}

void car_status_wifi_log(const char *msg)
{
    if (!msg) return;
    portENTER_CRITICAL(&s_cs_mux);
    strncpy(g_wifi_log[g_wifi_log_head], msg, sizeof(g_wifi_log[0]) - 1);
    g_wifi_log[g_wifi_log_head][sizeof(g_wifi_log[0]) - 1] = 0;
    g_wifi_log_head = (g_wifi_log_head + 1) % CAR_WIFI_LOG_MAX;
    if (g_wifi_log_cnt < CAR_WIFI_LOG_MAX) g_wifi_log_cnt++;
    portEXIT_CRITICAL(&s_cs_mux);
}

void car_status_set_ble_adv(int state)
{
    portENTER_CRITICAL(&s_cs_mux);
    g_ble_adv = state;
    portEXIT_CRITICAL(&s_cs_mux);
}

/* 把 src 拷进 out，转义 JSON 字符串里的 " 与 \，避免破坏 JSON */
static void json_escape(const char *src, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < out_len; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            out[j++] = '\\';
        }
        out[j++] = src[i];
    }
    out[j] = 0;
}

/* 带长度保护的追加：不保证完整（缓冲不足时截断），但绝不越界 */
static void cat(char *buf, size_t len, size_t *off, const char *fmt, ...)
{
    if (*off >= len) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, len - *off, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *off += (size_t)n;
        if (*off > len) *off = len;   /* 内容被截断，但缓冲安全 */
    }
}

void car_status_json(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    buf[0] = 0;

    char ip[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), "%d.%d.%d.%d", IP2STR(&info.ip));
        }
    }

    uint64_t uptime = esp_timer_get_time() / 1000000;

    const char *action;
    switch (car_get_action()) {
        case CAR_FORWARD:  action = "前进"; break;
        case CAR_BACKWARD: action = "后退"; break;
        case CAR_LEFT:     action = "左转"; break;
        case CAR_RIGHT:    action = "右转"; break;
        default:           action = "停止"; break;
    }

    /* 只复制标量/小量；避免在栈上放大数组（防止 httpd / NimBLE 任务栈溢出），
       扫描列表与日志在循环里逐个加锁复制单个元素 */
    char state[16];
    uint16_t scan_n;
    uint8_t wlog_cnt, wlog_head;
    int ble_adv;
    portENTER_CRITICAL(&s_cs_mux);
    strncpy(state, g_wifi_state, sizeof(state) - 1);
    state[sizeof(state) - 1] = 0;
    scan_n = g_scan_n;
    wlog_cnt = g_wifi_log_cnt;
    wlog_head = g_wifi_log_head;
    ble_adv = g_ble_adv;
    portEXIT_CRITICAL(&s_cs_mux);

    size_t off = 0;
    cat(buf, len, &off,
        "{\"ip\":\"%s\",\"uptime\":%llu,\"free_heap\":%lu,"
        "\"last_cmd\":\"%c\",\"action\":\"%s\",\"speed\":%d,"
        "\"cmd_count\":%lu,\"ble\":%d,\"ble_adv\":%d,"
        "\"wifi_state\":\"%s\",\"scan\":[",
        ip, (unsigned long long)uptime, (unsigned long)esp_get_free_heap_size(),
        car_get_last_cmd(), action, car_get_speed(),
        (unsigned long)car_get_cmd_count(), ble_car_is_connected() ? 1 : 0,
        ble_adv,
        state);

    char essid[40];
    scan_ap_t ap;
    for (uint16_t i = 0; i < scan_n; i++) {
        portENTER_CRITICAL(&s_cs_mux);
        memcpy(&ap, &g_scan[i], sizeof(ap));
        portEXIT_CRITICAL(&s_cs_mux);
        json_escape(ap.ssid, essid, sizeof(essid));
        cat(buf, len, &off,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"chan\":%u,\"auth\":%u,\"target\":%u}%s",
            essid, ap.rssi, ap.chan, ap.auth,
            ap.target, (i + 1 < scan_n) ? "," : "");
    }

    cat(buf, len, &off, "],\"wifi_log\":[");

    /* 按时间顺序输出（最旧 → 最新） */
    uint8_t start = (wlog_cnt < CAR_WIFI_LOG_MAX) ? 0 : wlog_head;
    char e[100];
    char line[96];
    for (uint8_t k = 0; k < wlog_cnt; k++) {
        uint8_t idx = (start + k) % CAR_WIFI_LOG_MAX;
        portENTER_CRITICAL(&s_cs_mux);
        strncpy(line, g_wifi_log[idx], sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        portEXIT_CRITICAL(&s_cs_mux);
        json_escape(line, e, sizeof(e));
        cat(buf, len, &off, "\"%s\"%s", e, (k + 1 < wlog_cnt) ? "," : "");
    }

    cat(buf, len, &off, "]}");
}
