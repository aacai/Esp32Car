#include "ble_car.h"
#include "motor.h"
#include "car_status.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "host/ble_hs_id.h"
#include <assert.h>
#include <stdio.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* NimBLE 自带的密钥仓库初始化实现；此处前向声明以避免额外引入 store 头文件 */
void ble_store_config_init(void);

static const char *TAG = "ble_car";

#define CAR_SERVICE_UUID 0xABCD
#define CAR_CMD_UUID    0x1234
#define CAR_STATUS_UUID 0x1235

static uint16_t g_cmd_handle;
static uint16_t g_status_handle;
static bool      g_ble_connected = false;
static char      g_ble_name[32] = "EspCar";
static uint8_t   g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static esp_timer_handle_t s_adv_retry_timer = NULL;

/* BLE 状态 JSON 复用的静态缓冲（通知与 GATT 读均在 NimBLE 主机任务内串行执行，可安全复用）；
   避免每次 8KB malloc 带来的长期堆碎片风险。 */
static char s_ble_json[8192];

static const ble_uuid16_t car_svc_uuid = BLE_UUID16_INIT(CAR_SERVICE_UUID);
static const ble_uuid16_t car_cmd_uuid = BLE_UUID16_INIT(CAR_CMD_UUID);
static const ble_uuid16_t car_status_uuid = BLE_UUID16_INIT(CAR_STATUS_UUID);

static int gatt_svr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gatt_svr_status_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&car_svc_uuid,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (ble_uuid_t *)&car_cmd_uuid,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .access_cb = gatt_svr_access,
                .val_handle = &g_cmd_handle,
            },
            {
                .uuid = (ble_uuid_t *)&car_status_uuid,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .access_cb = gatt_svr_status_access,
                .val_handle = &g_status_handle,
            },
            { 0 }
        }
    },
    { 0 }
};

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);

static void ble_car_notify_status(uint16_t conn_handle)
{
    if (!g_status_handle) {
        return;
    }
    /* 完整状态 JSON 可能超过 3KB，必须用堆分配足够大的缓冲，
       不能用栈上小数组（否则截断成残缺 JSON，客户端解析失败） */
    car_status_json(s_ble_json, sizeof(s_ble_json));
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_ble_json, strlen(s_ble_json));
    if (om) {
        ble_gatts_notify_custom(conn_handle, g_status_handle, om);
    }
}

static int gatt_svr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[16] = {0};
        uint16_t len = sizeof(buf) - 1;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc == 0 && len > 0) {
            ESP_LOGI(TAG, "BLE 指令: %.*s", len, buf);
            for (int i = 0; i < len; i++) {
                car_control((char)buf[i]);
            }
            ble_car_notify_status(conn_handle);
        }
        return 0;
    }
    return 0;
}

static int gatt_svr_status_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* NimBLE 内部已按客户端请求的 offset 自动切片（见 ble_gatts_val_access），
           回调只需把完整值 append 到 ctxt->om，无需自行处理偏移。 */
        car_status_json(s_ble_json, sizeof(s_ble_json));
        int rc = os_mbuf_append(ctxt->om, s_ble_json, strlen(s_ble_json));
        return rc == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    return;
}

static void ble_car_make_name(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        snprintf(g_ble_name, sizeof(g_ble_name),
                 "EspCar_%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
}

static void schedule_adv_retry(void)
{
    if (s_adv_retry_timer) {
        esp_timer_stop(s_adv_retry_timer);
        esp_timer_start_once(s_adv_retry_timer, 1000000); /* 1 秒后重试 */
    }
}

static void ble_app_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.name = (uint8_t *)g_ble_name;
    fields.name_len = strlen(g_ble_name);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播字段设置失败: %d", rc);
        car_status_set_ble_adv(rc);
        schedule_adv_retry();
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 第三个参数为广播持续时间：BLE_HS_FOREVER = 永久广播；
       传 0 会被 NimBLE 当作“持续 0ms”而立即结束广播 */
    rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "广播启动失败: %d（将重试）", rc);
        car_status_set_ble_adv(rc);
        schedule_adv_retry();
        return;
    }
    ESP_LOGI(TAG, "BLE 开始广播: %s", g_ble_name);
    car_status_set_ble_adv(0);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void on_sync(void)
{
    /* 使用出厂 public 地址（稳定、可被识别）。配合 Just Works 配对：
       安卓清空旧绑定后点连接，会用 Just Works 静默配对（无 PIN）；
       地址稳定，不会每次重启变成“新设备”，避免设备条目堆积与误连旧条目导致的“PIN 不正确”。 */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 地址准备失败: %d，放弃本轮广播", rc);
        return;
    }
    g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    ble_app_advertise();
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            g_ble_connected = true;
            ESP_LOGI(TAG, "BLE 已连接");
            ble_car_notify_status(event->connect.conn_handle);
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            g_ble_connected = false;
            ESP_LOGI(TAG, "BLE 已断开，重新广播");
            ble_app_advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ble_app_advertise();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            /* 配对/加密完成时触发：status=0 表示成功（Just Works 静默完成）。
               若安卓仍报“拒绝配对/密钥不对”，看这条日志即可定位。 */
            ESP_LOGI(TAG, "加密/配对状态变化: status=%d", event->enc_change.status);
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            /* 已配对设备再次发起配对：删除冲突绑定并继续配对，避免被拒 */
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        default:
            break;
    }
    return 0;
}

static void adv_retry_cb(void *arg)
{
    ble_app_advertise();
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task 启动");
    nimble_port_run();
}

void ble_car_init(void)
{
    /* 创建广播失败后的重试定时器（不依赖 NimBLE 任务，可随时启动） */
    esp_timer_create_args_t tc = {
        .callback = adv_retry_cb,
        .name = "ble_adv_retry"
    };
    esp_timer_create(&tc, &s_adv_retry_timer);

    /* 不要在这里手动调 esp_bt_controller_init/enable：
       nimble_port_init() 内部已经会做（见 nimble_port.c），
       提前初始化会让它内部那次调用返回 ESP_ERR_INVALID_STATE(0x103)，
       导致 nimble_port_init 整体失败、BLE 主机根本不启动。
       官方 bleprph 示例同样只调 nimble_port_init()。 */
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init 失败: 0x%x", rc);
        car_status_set_ble_adv(rc);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Just Works 配对配置：无 I/O 能力(IO_CAP=NONE) -> 自动走 Just Works，无需 PIN。
       sm_bonding=1 允许绑定，密钥(ENC+ID)持久化到 NVS。
       注意：用 Legacy(非 SC) 以获得最佳安卓/MIUI 兼容性——SC 在部分安卓上会在加密阶段
       报 “Connection Rejected Due To Security Reasons”(0x0D)，导致绑定 30s 超时失败。
       Legacy Just Works 同样无需 PIN，且所有安卓版本都支持。
       MIUI 连接时会强制发起配对，车端必须能响应并完成 SMP，否则报“已拒绝配对/密钥不对”。
       GATT 特征本身不要求加密，所以纯连接(不配对)也能收发 F/B/L/R/S —— 两种用法都支持。 */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "ble_gatts_count_cfg 失败: %d", rc); return; }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "ble_gatts_add_svcs 失败: %d", rc); return; }

    ble_car_make_name();
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(g_ble_name);
    ble_svc_gatt_init();

    /* 初始化密钥仓库（必须调用，否则配对时无法保存/查找 LTK，SMP 会立即失败）。
       需配合 store_status_cb 与 sdkconfig 的 SECURITY_ENABLE/NVS_PERSIST。 */
    ble_store_config_init();

    nimble_port_freertos_init(ble_host_task);
}

bool ble_car_is_connected(void)
{
    return g_ble_connected;
}
