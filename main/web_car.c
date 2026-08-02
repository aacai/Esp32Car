#include "web_car.h"
#include "motor.h"
#include "ble_car.h"
#include "car_status.h"
#include "car_log.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_ota_ops.h"

extern const int ENABLE_CAPTIVE_PORTAL;   /* 在 main.c 中定义，控制是否启用 Captive Portal */

static const char *TAG = "web_car";

/* 由 EMBED_FILES 嵌入的 index.html */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* /status、/mylog 共用的静态 JSON 缓冲。esp_http_server 单任务串行处理请求，
   可安全复用同一块；改用静态缓冲避免每次请求 malloc 8KB（长期堆碎片/偶发失败）。 */
static char s_web_json[8192];

static esp_err_t root_handler(httpd_req_t *req)
{
    const uint32_t len = (uint32_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, (const char *)index_html_start, len);
    return ESP_OK;
}

static esp_err_t api_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof(buf)) {
        httpd_req_get_url_query_str(req, buf, sizeof(buf));
        char *p = strstr(buf, "c=");
        if (p) {
            for (char *q = p + 2; *q && *q != '&'; q++) {
                car_control(*q);
                ESP_LOGI(TAG, "Web 指令: %c", *q);
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"ok\":1}", 7);
    return ESP_OK;
}

/* 调试状态接口：返回小车实时状态 JSON */
static esp_err_t status_handler(httpd_req_t *req)
{
    /* 完整状态 JSON 含扫描列表与日志，可能超过 3KB；用静态缓冲（见 s_web_json），
       避免每次请求 malloc 8KB 带来的长期堆碎片与偶发分配失败。 */
    car_status_json(s_web_json, sizeof(s_web_json));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, s_web_json, strlen(s_web_json));
    return ESP_OK;
}

/* 固件运行日志接口：GET /mylog?from=N 增量拉取，N 传上次返回的 next。
   浏览器网页与命令行 curl 共用，串口不可用时这是主要观测手段。 */
static esp_err_t log_handler(httpd_req_t *req)
{
    uint32_t from = 0;
    char q[48] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < sizeof(q)) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        char v[16] = {0};
        if (httpd_query_key_value(q, "from", v, sizeof(v)) == ESP_OK) {
            from = (uint32_t)strtoul(v, NULL, 10);
        }
    }

    car_log_json(s_web_json, sizeof(s_web_json), from);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, s_web_json, strlen(s_web_json));
    return ESP_OK;
}

/* ---------- 固件在线升级（OTA）----------
   POST /update 直接接收原始 .bin 固件字节（不带 multipart），流式写入另一 OTA 分区，
   写完后设置启动分区并重启。前端用 XHR 上传文件原始内容，可显示进度。 */
#define OTA_BUF_SIZE   1024
#define OTA_MAX_RETRY  50

static esp_err_t update_handler(httpd_req_t *req)
{
    const esp_partition_t *ota_part = esp_ota_get_next_update_partition(NULL);
    if (ota_part == NULL) {
        ESP_LOGE(TAG, "找不到可用的 OTA 分区（分区表缺少 ota_0/ota_1？）");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }

    const int total = (int)req->content_len;
    if (total <= 0 || total > (int)ota_part->size) {
        ESP_LOGE(TAG, "OTA 固件大小 %d 超出分区 %d", total, (int)ota_part->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware too large");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(ota_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %d", err);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_end(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0, retries = 0;
    while (received < total) {
        int need = (int)(total - received);
        if (need > OTA_BUF_SIZE) need = OTA_BUF_SIZE;
        int r = httpd_req_recv(req, (char *)buf, need);
        if (r == 0) break;                               /* 对端关闭连接 */
        if (r < 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && retries++ < OTA_MAX_RETRY) continue;
            ESP_LOGE(TAG, "OTA 接收失败: %d", r);
            free(buf); esp_ota_end(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write 失败: %d", err);
            free(buf); esp_ota_end(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write failed");
            return ESP_FAIL;
        }
        received += r;
    }
    free(buf);

    if (received != total) {
        ESP_LOGE(TAG, "OTA 接收不完整 %d/%d", received, total);
        esp_ota_end(ota_handle);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete upload");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end 失败（固件校验不通过？）: %d", err);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end failed");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(ota_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败: %d", err);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA 升级完成，目标分区 %s，准备重启", ota_part->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"ok\":1,\"msg\":\"upgrade-ok-reboot\"}", 30);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static httpd_handle_t s_server = NULL;

/* ---------- Captive Portal（强制门户 / 认证页自动弹出） ----------
   1) DNS 劫持：把连入热点的设备发出的所有域名解析都指回小车自身 IP，
      这样手机系统的「连通性检测」会被引到本地，从而弹出认证页。
   2) HTTP 通配 302：对任意非页面路径的请求返回 302 跳回控制页，
      连通性检测拿不到预期的 204/Success，即判定为需要登录。 */
#define CP_DNS_PORT   53
#define CP_DNS_BUF    512
#define CP_PORTAL_URL "http://192.168.4.1/"

/* 把软 AP 的 IP 写死（ESP-IDF softAP 默认即 192.168.4.1，与 main.c 注释一致） */
static const uint8_t s_ap_ip[4] = { 192, 168, 4, 1 };

static void dns_server_task(void *arg)
{
    (void)arg;
    uint8_t rx[CP_DNS_BUF];
    struct sockaddr_in saddr = {0};
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port        = htons(CP_DNS_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "CP DNS socket 创建失败"); vTaskDelete(NULL); return; }
    if (bind(sock, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        ESP_LOGE(TAG, "CP DNS bind 失败");
        close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "Captive Portal DNS 已启动 :53 -> 192.168.4.1");

    struct sockaddr_in caddr;
    socklen_t clen;
    while (1) {
        clen = sizeof(caddr);
        int n = recvfrom(sock, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&caddr, &clen);
        if (n < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }  /* socket 出错，稍等避免忙转 */
        if (n <= 12) continue;                                   /* 短于首部，忽略 */
        uint16_t qd = ((uint16_t) rx[4] << 8) | rx[5];
        if (qd == 0) continue;                                   /* 无问题，忽略 */

        /* 定位第一个 question 的结尾（name 可能含压缩指针） */
        int p = 12;
        while (p < n && rx[p] != 0) {
            int l = rx[p];
            if ((l & 0xC0) == 0xC0) { p += 2; break; }
            p += l + 1;
        }
        if (p >= n) continue;
        p += 1;                                                   /* 跳过 name 终止 0 */
        int qend = p + 4;                                        /* 跳过 QTYPE + QCLASS */
        if (qend > n) continue;

        uint8_t tx[CP_DNS_BUF];
        int t = 0;
        tx[t++] = rx[0]; tx[t++] = rx[1];                         /* 回显事务 ID */
        tx[t++] = 0x81; tx[t++] = 0x80;                          /* QR=1 AA=1 RD=1 RA=1 */
        tx[t++] = 0x00; tx[t++] = qd;                            /* QDCOUNT */
        tx[t++] = 0x00; tx[t++] = 0x01;                          /* ANCOUNT = 1 */
        tx[t++] = 0x00; tx[t++] = 0x00;                          /* NSCOUNT */
        tx[t++] = 0x00; tx[t++] = 0x00;                          /* ARCOUNT */
        memcpy(tx + t, rx + 12, qend - 12); t += (qend - 12);    /* 回显 question */
        tx[t++] = 0xC0; tx[t++] = 0x0C;                          /* name 指针 -> 0x0C */
        tx[t++] = 0x00; tx[t++] = 0x01;                          /* TYPE A */
        tx[t++] = 0x00; tx[t++] = 0x01;                          /* CLASS IN */
        tx[t++] = 0x00; tx[t++] = 0x00; tx[t++] = 0x00; tx[t++] = 0x00; /* TTL 0 */
        tx[t++] = 0x00; tx[t++] = 0x04;                          /* RDLENGTH = 4 */
        tx[t++] = s_ap_ip[0]; tx[t++] = s_ap_ip[1];
        tx[t++] = s_ap_ip[2]; tx[t++] = s_ap_ip[3];              /* A 记录 = 小车 IP */

        sendto(sock, tx, t, 0, (struct sockaddr *)&caddr, clen);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* 通配跳转：把连通性检测等任意非页面请求重定向到控制页，触发认证弹窗 */
static esp_err_t catchall_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", CP_PORTAL_URL);
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

void web_car_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (ENABLE_CAPTIVE_PORTAL)
        config.uri_match_fn = httpd_uri_match_wildcard;   /* 支持通配 URI，未匹配的请求走认证跳转 */

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_server, &root);

        httpd_uri_t api = {
            .uri = "/api",
            .method = HTTP_GET,
            .handler = api_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_server, &api);

        httpd_uri_t status = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_server, &status);

        httpd_uri_t logu = {
            .uri = "/mylog",
            .method = HTTP_GET,
            .handler = log_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_server, &logu);

        /* 固件在线升级（OTA）：POST 原始 .bin 固件，设备写入另一 OTA 分区后重启。
           务必在通配 catchall 之前注册，使 /update 精确匹配优先于通配 URI 跳转。 */
        httpd_uri_t ota = {
            .uri = "/update",
            .method = HTTP_POST,
            .handler = update_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_server, &ota);

        if (ENABLE_CAPTIVE_PORTAL) {
            /* 通配跳转：未匹配的具体接口（含系统连通性检测地址）一律 302 到控制页 */
            httpd_uri_t catchall = {
                .uri      = "/*",
                .method   = HTTP_ANY,
                .handler  = catchall_handler,
                .user_ctx = NULL,
            };
            httpd_register_uri_handler(s_server, &catchall);

            /* 启动 Captive Portal DNS 劫持，使连接设备自动弹出认证/控制页 */
            xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 5, NULL);
        }

        ESP_LOGI(TAG, "Web 遥控已启动，浏览器打开 http://192.168.4.1/ ，日志接口 /mylog");
    } else {
        ESP_LOGE(TAG, "HTTP server 启动失败");
    }
}
