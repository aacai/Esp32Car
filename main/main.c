#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "esp_mac.h"

#include "motor.h"
#include "web_car.h"
#include "ble_car.h"
#include "car_status.h"
#include "car_log.h"

static const char *TAG = "car_main";

#define WIFI_SSID  CONFIG_EXAMPLE_WIFI_SSID
#define WIFI_PASS  CONFIG_EXAMPLE_WIFI_PASSWORD

/* 是否启用 Captive Portal（连上 WiFi 自动弹出认证/控制页）。
   1 = 启用；0 = 关闭（关闭后为普通无密码热点，需手动打开 http://192.168.4.1/）。
   改这一个常量即可，无需进 menuconfig。web_car.c 通过 extern 引用。 */
const int ENABLE_CAPTIVE_PORTAL = 1;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static EventGroupHandle_t s_wifi_event_group;
static int s_retry = 0;

/* WiFi 初始化专用的容错检查：出错只放弃 WiFi 任务，绝不 abort 整机。
   （历史事故：这里用 ESP_ERROR_CHECK 导致一次 WiFi 配置错误让板子无限重启，
     连带蓝牙和网页一起失效，且因串口不可见而长时间无法定位） */
#define WIFI_TRY(x) do {                                                      \
        esp_err_t _e = (x);                                                   \
        if (_e != ESP_OK) {                                                   \
            ESP_LOGE(TAG, "WiFi 初始化失败 [%s] err=0x%x", #x, _e);            \
            char _m[80];                                                      \
            snprintf(_m, sizeof(_m), "WiFi 初始化失败 0x%x @%d", _e, __LINE__);\
            car_status_wifi_log(_m);                                          \
            car_status_set_wifi_state("error");                               \
            vTaskDelete(NULL);                                                \
        }                                                                     \
    } while (0)

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 仅标记启动；真正的连接由 wifi_init_sta 扫描完成后统一发起，
           避免 STA_START 与扫描并发触发 connect（看不到目标就先连会失败重试） */
        car_status_set_wifi_state("connecting");
        car_status_wifi_log("WiFi 已启动，准备扫描");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi 断开! reason=%d (参考 ESP-IDF reason code 表), ssid=%.32s, rssi=%d",
                 d->reason, d->ssid, d->rssi);
        char rm[64];
        snprintf(rm, sizeof(rm), "WiFi 断开 reason=%d", d->reason);
        car_status_wifi_log(rm);
        car_status_set_wifi_state("disconnected");
        if (s_retry < 5) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "WiFi 重连 (%d/5)", s_retry);
            car_status_wifi_log("WiFi 断开，重连中...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            car_status_wifi_log("WiFi 重连失败，已放弃");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        char ipbuf[16];
        snprintf(ipbuf, sizeof(ipbuf), "%d.%d.%d.%d", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi 已连接，IP=%s", ipbuf);
        car_status_set_wifi_state("connected");
        char msg[64];
        snprintf(msg, sizeof(msg), "WiFi 已连接，IP=%s", ipbuf);
        car_status_wifi_log(msg);
    }
}

static void print_scan_result(uint16_t ap_count, wifi_ap_record_t *ap_list)
{
    ESP_LOGI(TAG, "扫到 %d 个 WiFi：", ap_count);
    for (int i = 0; i < ap_count; i++) {
        const char *auth;
        switch (ap_list[i].authmode) {
            case WIFI_AUTH_OPEN:       auth = "OPEN"; break;
            case WIFI_AUTH_WEP:        auth = "WEP"; break;
            case WIFI_AUTH_WPA_PSK:    auth = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK:   auth = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA3_PSK:   auth = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth = "WPA2/WPA3"; break;
            default:                   auth = "?"; break;
        }
        uint8_t is_target = (strcmp((char *)ap_list[i].ssid, WIFI_SSID) == 0) ? 1 : 0;
        car_status_set_scan_ap(i, (char *)ap_list[i].ssid, ap_list[i].rssi,
                               ap_list[i].primary, ap_list[i].authmode, is_target);
        ESP_LOGI(TAG, "  [%2d]%s %-32s RSSI=%4d  chan=%2d  auth=%s",
                 i + 1, is_target ? "★" : " ", ap_list[i].ssid,
                 ap_list[i].rssi, ap_list[i].primary, auth);
    }
}

/* 扫描附近 WiFi。找到目标 SSID 返回 true，否则 false。
   调用前需已 esp_wifi_init + esp_wifi_set_mode(STA) + esp_wifi_start。 */
static bool wifi_scan_and_find(const char *target_ssid)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    esp_err_t serr = esp_wifi_scan_start(&scan_cfg, true);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "扫描启动失败 err=0x%x（跳过本轮）", serr);
        car_status_wifi_log("扫描启动失败，跳过本轮");
        return false;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    car_status_set_scan_count(ap_count);

    wifi_ap_record_t *ap_list = NULL;
    if (ap_count > 0) {
        ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            print_scan_result(ap_count, ap_list);
        }
    } else {
        ESP_LOGI(TAG, "未发现任何 WiFi（可能范围内无 AP）");
    }

    bool found = false;
    for (uint16_t i = 0; i < ap_count && ap_list; i++) {
        if (strcmp((char *)ap_list[i].ssid, target_ssid) == 0) {
            found = true;
            break;
        }
    }

    if (ap_list) {
        free(ap_list);
    }
    return found;
}

static void wifi_connect_target(void)
{
    wifi_config_t w = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strncpy((char *)w.sta.ssid, WIFI_SSID, sizeof(w.sta.ssid) - 1);
    strncpy((char *)w.sta.password, WIFI_PASS, sizeof(w.sta.password) - 1);

    car_status_set_wifi_state("connecting");
    car_status_wifi_log("正在连接目标 WiFi...");
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &w);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "STA 配置失败 err=0x%x", e);
        car_status_wifi_log("STA 配置失败");
        return;
    }
    e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "发起连接失败 err=0x%x（等待下轮重试）", e);
        car_status_wifi_log("发起连接失败，等待重试");
    }
}

static void wifi_init_sta(void *arg)
{
    s_wifi_event_group = xEventGroupCreate();

    WIFI_TRY(esp_netif_init());
    WIFI_TRY(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    WIFI_TRY(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_got_ip;
    WIFI_TRY(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, &inst_any));
    WIFI_TRY(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, &inst_got_ip));

    /* 必须先切到 APSTA 模式，再配置 AP 接口。
       顺序反了会让 esp_wifi_set_config(WIFI_IF_AP,…) 返回 ESP_ERR_WIFI_MODE(0x3005) */
    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* 同时开启板子自带 AP（EspCar_XXXXXX，固定 IP 192.168.4.1），
       便于手机/电脑随时连入查看网页状态与控制，无需先连上外部 WiFi */
    char ap_name[32];
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        snprintf(ap_name, sizeof(ap_name), "EspCar_%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        strcpy(ap_name, "EspCar");
    }
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, ap_name, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ap_name);
    ap_cfg.ap.password[0] = '\0';
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    WIFI_TRY(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    WIFI_TRY(esp_wifi_start());

    /* 先扫描，确认目标 SSID 真实存在再连接 */
    car_status_set_wifi_state("scanning");
    car_status_wifi_log("扫描附近 WiFi...");
    ESP_LOGI(TAG, "目标 WiFi: SSID=%s", WIFI_SSID);
    bool found = wifi_scan_and_find(WIFI_SSID);
    if (!found) {
        ESP_LOGW(TAG, "扫描未找到 SSID=\"%s\"，稍后重试（确认路由器 2.4G 已开、SSID 未隐藏、板子在覆盖范围内）",
                 WIFI_SSID);
        car_status_wifi_log("未找到目标 SSID，稍后周期重试");
    } else {
        car_status_wifi_log("已找到目标 SSID，连接中");
    }

    /* 找到目标就连接；没找到也尝试连一次（扫描可能漏掉），连不上会按 STA_DISCONNECTED 处理 */
    wifi_connect_target();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi 首次连接成功");
    } else {
        ESP_LOGW(TAG, "WiFi 首次连接失败，将由重连任务继续重试");
    }
    /* 关键：wifi_init_sta 是作为 FreeRTOS 任务运行的，任务函数绝不能 return，
       否则 FreeRTOS 会触发 "wifi_init should not return, Aborting now!" 并整机重启。
       这里主动删除自己，把连接维持交给事件回调与重连任务。 */
    vTaskDelete(NULL);
}

static void wifi_scan_reconnect_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));   /* 每 30 秒重扫一次 */

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            continue;   /* 已连接，跳过 */
        }

        car_status_set_wifi_state("scanning");
        car_status_wifi_log("周期扫描附近 WiFi...");
        bool found = wifi_scan_and_find(WIFI_SSID);
        if (found) {
            ESP_LOGI(TAG, "发现目标 SSID，尝试连接 ...");
            car_status_wifi_log("发现目标 SSID，尝试连接");
            s_retry = 0;
            wifi_connect_target();
        }
    }
}

void app_main(void)
{
    /* 第一件事：接管日志输出，之后所有 ESP_LOGx 都会留一份在内存里，
       网页 /mylog 可实时读取（Mac 上串口易误入下载模式，这是主要观测手段） */
    car_log_init();

    ESP_LOGI(TAG, "ESP32-C3 小车启动");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    motor_init();

    /* WiFi 初始化放进独立任务，避免其内部的连接等待阻塞 BLE / Web 的启动：
       无论外部 WiFi 是否连上，蓝牙与网页都能一开机即用 */
    xTaskCreate(wifi_init_sta, "wifi_init", 4096, NULL, 4, NULL);
    /* 周期重新扫描：一旦范围内出现目标 SSID 即重新连接
       （适用于板子先上电、后靠近路由器的情况），首轮延迟 30s，此时 WiFi 早已就绪 */
    xTaskCreate(wifi_scan_reconnect_task, "wifi_scan", 4096, NULL, 3, NULL);

#if CONFIG_ENABLE_BLE
    ble_car_init();
#endif

    web_car_start();

    ESP_LOGI(TAG, "控制指令: F前进 B后退 L左转 R右转 S停止  数字0-9设速度(0~90%%)");

}
