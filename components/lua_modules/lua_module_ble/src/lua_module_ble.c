/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_ble.h"

#include <stdbool.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "lauxlib.h"
#include "lua.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define LUA_MODULE_BLE_NAME "ble"

#define LUA_BLE_GAP_SVC_UUID      0xfff0
#define LUA_BLE_GAP_CHR_UUID      0xfff1
#define LUA_BLE_DEFAULT_NAME      "esp-claw"
#define LUA_BLE_ADV_NAME_MAX      29
#define LUA_BLE_ADV_UUID16_MAX    4
#define LUA_BLE_GATT_VALUE_MAX    64
#define LUA_BLE_HOST_SYNC_TIMEOUT_MS 15000
#define LUA_BLE_NOTIFY_SUBSCRIPTION_MAX 8

#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
#define LUA_BLE_DEFAULT_ADV_UUID16_COUNT 1
#else
#define LUA_BLE_DEFAULT_ADV_UUID16_COUNT 0
#endif

static const char *TAG = "lua_ble";

typedef struct {
    uint16_t attr_handle;
    bool notify_enabled;
} lua_ble_notify_subscription_t;

static void lua_ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static SemaphoreHandle_t s_host_sync_sem;
static bool s_stack_inited;
static bool s_host_synced;
static bool s_controller_inited_by_ble;
static bool s_controller_enabled_by_ble;
static bool s_nimble_inited;
static bool s_host_task_started;
static bool s_advertising;
static bool s_advertising_requested;
static bool s_scanning;
static uint8_t s_own_addr_type;
#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
static uint16_t s_gatt_val_handle;
#endif
static lua_module_ble_connection_t s_conn = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};
static lua_ble_notify_subscription_t s_notify_subscriptions[LUA_BLE_NOTIFY_SUBSCRIPTION_MAX];
static char s_adv_name[LUA_BLE_ADV_NAME_MAX + 1] = LUA_BLE_DEFAULT_NAME;
static uint16_t s_adv_appearance;
static uint16_t s_adv_uuid16_list[LUA_BLE_ADV_UUID16_MAX] = {
    LUA_BLE_GAP_SVC_UUID,
};
static size_t s_adv_uuid16_count = LUA_BLE_DEFAULT_ADV_UUID16_COUNT;
#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
static char s_gatt_value[LUA_BLE_GATT_VALUE_MAX] = "hello";
#endif
static ble_hs_reset_fn *s_external_reset_cb;
static ble_hs_sync_fn *s_external_sync_cb;
static ble_gatt_register_fn *s_external_gatts_register_cb;
static void *s_external_gatts_register_arg;

typedef int (*lua_ble_gap_event_handler_t)(struct ble_gap_event *event);

typedef struct {
    int type;
    lua_ble_gap_event_handler_t handler;
} lua_ble_gap_event_handler_entry_t;

static int lua_ble_gap_event(struct ble_gap_event *event, void *arg);
static int lua_ble_dispatch_gap_event(struct ble_gap_event *event);
static void lua_ble_on_reset(int reason);
static void lua_ble_on_sync(void);
static esp_err_t lua_ble_nvs_init(void);
static esp_err_t lua_ble_controller_start(void);
static void lua_ble_controller_stop(void);
static esp_err_t lua_ble_adv_apply(const char *name, uint16_t appearance, const uint16_t *uuid16_list,
                                   size_t uuid16_count);
#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
static int lua_ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
#endif
static esp_err_t lua_ble_stack_stop(void);

#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(LUA_BLE_GAP_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(LUA_BLE_GAP_CHR_UUID),
                .access_cb = lua_ble_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_gatt_val_handle,
            },
            {
                0,
            },
        },
    },
    {
        0,
    },
};
#endif

static void lua_ble_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "GATT service registered handle=%d uuid=%s",
                 ctxt->svc.handle,
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf));
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG, "GATT characteristic def_handle=%d val_handle=%d uuid=%s",
                 ctxt->chr.def_handle,
                 ctxt->chr.val_handle,
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf));
        break;
    default:
        break;
    }
}

static void lua_ble_chained_reset_cb(int reason)
{
    lua_ble_on_reset(reason);
    if (s_external_reset_cb) {
        s_external_reset_cb(reason);
    }
}

static void lua_ble_chained_sync_cb(void)
{
    lua_ble_on_sync();
    if (s_external_sync_cb) {
        s_external_sync_cb();
    }
}

static void lua_ble_chained_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    lua_ble_gatts_register_cb(ctxt, arg);
    if (s_external_gatts_register_cb) {
        s_external_gatts_register_cb(ctxt, s_external_gatts_register_arg);
    }
}

static esp_err_t lua_ble_stack_prepare(void)
{
    esp_err_t err;

    if (s_nimble_inited) {
        return ESP_OK;
    }

    err = lua_ble_nvs_init();
    if (err != ESP_OK) {
        return err;
    }

    if (s_host_sync_sem == NULL) {
        s_host_sync_sem = xSemaphoreCreateBinary();
        if (s_host_sync_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = lua_ble_controller_start();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_init failed: %s", esp_err_to_name(err));
        lua_ble_controller_stop();
        return err;
    }
    s_nimble_inited = true;
    return ESP_OK;
}

#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
static int lua_ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;

    (void)conn_handle;
    (void)arg;

    if (attr_handle != s_gatt_val_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        size_t len = strnlen(s_gatt_value, sizeof(s_gatt_value));

        rc = os_mbuf_append(ctxt->om, s_gatt_value, len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        int om_len = OS_MBUF_PKTLEN(ctxt->om);

        if (om_len < 0 || (size_t)om_len >= sizeof(s_gatt_value)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        rc = os_mbuf_copydata(ctxt->om, 0, om_len, s_gatt_value);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_gatt_value[om_len] = '\0';
        (void)lua_module_ble_notify(attr_handle, s_gatt_value, (size_t)om_len);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}
#endif

static void lua_ble_notify_subscriptions_clear(void)
{
    memset(s_notify_subscriptions, 0, sizeof(s_notify_subscriptions));
}

static void lua_ble_notify_subscriptions_refresh_summary(void)
{
    s_conn.notify_subscribed = false;
    for (size_t i = 0; i < LUA_BLE_NOTIFY_SUBSCRIPTION_MAX; i++) {
        if (s_notify_subscriptions[i].notify_enabled) {
            s_conn.notify_subscribed = true;
            return;
        }
    }
}

static void lua_ble_notify_subscription_update(uint16_t attr_handle, bool notify_enabled)
{
    size_t empty_index = LUA_BLE_NOTIFY_SUBSCRIPTION_MAX;

    if (attr_handle == 0) {
        return;
    }

    for (size_t i = 0; i < LUA_BLE_NOTIFY_SUBSCRIPTION_MAX; i++) {
        if (s_notify_subscriptions[i].attr_handle == attr_handle) {
            s_notify_subscriptions[i].notify_enabled = notify_enabled;
            lua_ble_notify_subscriptions_refresh_summary();
            return;
        }
        if (empty_index == LUA_BLE_NOTIFY_SUBSCRIPTION_MAX &&
            s_notify_subscriptions[i].attr_handle == 0) {
            empty_index = i;
        }
    }

    if (!notify_enabled) {
        lua_ble_notify_subscriptions_refresh_summary();
        return;
    }

    if (empty_index == LUA_BLE_NOTIFY_SUBSCRIPTION_MAX) {
        ESP_LOGW(TAG, "notify subscription table full attr_handle=%u", attr_handle);
        lua_ble_notify_subscriptions_refresh_summary();
        return;
    }

    s_notify_subscriptions[empty_index].attr_handle = attr_handle;
    s_notify_subscriptions[empty_index].notify_enabled = true;
    lua_ble_notify_subscriptions_refresh_summary();
}

bool lua_module_ble_is_notify_subscribed(uint16_t attr_handle)
{
    if (attr_handle == 0) {
        return false;
    }

    for (size_t i = 0; i < LUA_BLE_NOTIFY_SUBSCRIPTION_MAX; i++) {
        if (s_notify_subscriptions[i].attr_handle == attr_handle) {
            return s_notify_subscriptions[i].notify_enabled;
        }
    }
    return false;
}

static void lua_ble_connection_clear(void)
{
    s_conn.connected = false;
    s_conn.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_conn.encrypted = false;
    s_conn.bonded = false;
    s_conn.notify_subscribed = false;
    lua_ble_notify_subscriptions_clear();
}

static void lua_ble_connection_update_security(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int rc;

    rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "connection security lookup failed conn_handle=%u rc=%d", conn_handle, rc);
        return;
    }

    s_conn.encrypted = desc.sec_state.encrypted;
    s_conn.bonded = desc.sec_state.bonded;

    ESP_LOGI(TAG, "connection security conn_handle=%u encrypted=%d bonded=%d authenticated=%d key_size=%u",
             conn_handle,
             s_conn.encrypted,
             s_conn.bonded,
             desc.sec_state.authenticated,
             desc.sec_state.key_size);
}

static void lua_ble_connection_set_connected(uint16_t conn_handle)
{
    s_conn.connected = true;
    s_conn.conn_handle = conn_handle;
    s_conn.notify_subscribed = false;
    lua_ble_notify_subscriptions_clear();
    lua_ble_connection_update_security(conn_handle);
}

esp_err_t lua_module_ble_notify(uint16_t attr_handle, const void *data, size_t len)
{
    struct os_mbuf *om;
    int rc;

    if (!s_conn.connected) {
        ESP_LOGW(TAG, "notify skipped: not connected attr_handle=%u", attr_handle);
        return ESP_ERR_INVALID_STATE;
    }
    if (!lua_module_ble_is_notify_subscribed(attr_handle)) {
        ESP_LOGW(TAG, "notify skipped: not subscribed conn_handle=%u attr_handle=%u",
                 s_conn.conn_handle,
                 attr_handle);
        return ESP_ERR_INVALID_STATE;
    }

    om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "notify failed: no mbuf conn_handle=%u attr_handle=%u len=%u",
                 s_conn.conn_handle,
                 attr_handle,
                 (unsigned int)len);
        return ESP_ERR_NO_MEM;
    }

    rc = ble_gatts_notify_custom(s_conn.conn_handle, attr_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "notify failed conn_handle=%u attr_handle=%u rc=%d",
                 s_conn.conn_handle,
                 attr_handle,
                 rc);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "notify sent conn_handle=%u attr_handle=%u len=%u",
             s_conn.conn_handle,
             attr_handle,
             (unsigned int)len);
    return ESP_OK;
}

static void lua_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset reason=%d", reason);
}

static void lua_ble_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
    }

    s_host_synced = true;
    if (s_host_sync_sem) {
        (void)xSemaphoreGive(s_host_sync_sem);
    }
}

static int lua_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    return lua_ble_dispatch_gap_event(event);
}

static int lua_ble_handle_gap_connect(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap connect conn_handle=%u status=%d",
             event->connect.conn_handle,
             event->connect.status);
    if (event->connect.status == 0) {
        s_advertising = false;
        lua_ble_connection_set_connected(event->connect.conn_handle);
    } else {
        s_conn.connected = false;
    }
    return 0;
}

static int lua_ble_handle_gap_disconnect(struct ble_gap_event *event)
{
    int rc;

    ESP_LOGI(TAG, "gap disconnect conn_handle=%u reason=%d",
             event->disconnect.conn.conn_handle,
             event->disconnect.reason);
    lua_ble_connection_clear();
    if (s_advertising_requested) {
        rc = lua_ble_adv_apply(s_adv_name,
                               s_adv_appearance,
                               s_adv_uuid16_list,
                               s_adv_uuid16_count);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "restart advertising after disconnect failed: %s", esp_err_to_name(rc));
        }
    }
    return 0;
}

static int lua_ble_handle_gap_disc(struct ble_gap_event *event)
{
    int rc;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
    if (rc != 0) {
        return 0;
    }
    if (fields.name_len > 0 && fields.name != NULL) {
        ESP_LOGI(TAG, "scan mac=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d name=%.*s",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0],
                 event->disc.rssi,
                 (int)fields.name_len, (const char *)fields.name);
    } else {
        ESP_LOGI(TAG, "scan mac=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d name=",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0],
                 event->disc.rssi);
    }
    return 0;
}

static int lua_ble_handle_gap_disc_complete(struct ble_gap_event *event)
{
    (void)event;

    ESP_LOGI(TAG, "scan complete");
    s_scanning = false;
    return 0;
}

static int lua_ble_handle_gap_adv_complete(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap adv_complete reason=%d", event->adv_complete.reason);
    s_advertising = false;
    return 0;
}

static int lua_ble_handle_gap_enc_change(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap enc_change conn_handle=%u status=%d",
             event->enc_change.conn_handle,
             event->enc_change.status);
    if (event->enc_change.status == 0) {
        lua_ble_connection_update_security(event->enc_change.conn_handle);
    }
    return 0;
}

static int lua_ble_handle_gap_identity_resolved(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap identity_resolved conn_handle=%u", event->identity_resolved.conn_handle);
    lua_ble_connection_update_security(event->identity_resolved.conn_handle);
    return 0;
}

static int lua_ble_handle_gap_repeat_pairing(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap repeat_pairing conn_handle=%u key_size=%u authenticated=%u sc=%u",
             event->repeat_pairing.conn_handle,
             event->repeat_pairing.cur_key_size,
             event->repeat_pairing.cur_authenticated,
             event->repeat_pairing.cur_sc);
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
}

static int lua_ble_handle_gap_subscribe(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap subscribe conn_handle=%u attr_handle=%u reason=%u prev_notify=%u cur_notify=%u "
             "prev_indicate=%u cur_indicate=%u",
             event->subscribe.conn_handle,
             event->subscribe.attr_handle,
             event->subscribe.reason,
             event->subscribe.prev_notify,
             event->subscribe.cur_notify,
             event->subscribe.prev_indicate,
             event->subscribe.cur_indicate);
    if (s_conn.connected && event->subscribe.conn_handle == s_conn.conn_handle) {
        lua_ble_notify_subscription_update(event->subscribe.attr_handle,
                                           event->subscribe.cur_notify);
    }
    return 0;
}

static int lua_ble_handle_gap_mtu(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap mtu conn_handle=%u channel_id=%u mtu=%u",
             event->mtu.conn_handle,
             event->mtu.channel_id,
             event->mtu.value);
    return 0;
}

static const lua_ble_gap_event_handler_entry_t s_gap_event_handlers[] = {
    { BLE_GAP_EVENT_CONNECT, lua_ble_handle_gap_connect },
    { BLE_GAP_EVENT_DISCONNECT, lua_ble_handle_gap_disconnect },
    { BLE_GAP_EVENT_DISC, lua_ble_handle_gap_disc },
    { BLE_GAP_EVENT_DISC_COMPLETE, lua_ble_handle_gap_disc_complete },
    { BLE_GAP_EVENT_ADV_COMPLETE, lua_ble_handle_gap_adv_complete },
    { BLE_GAP_EVENT_ENC_CHANGE, lua_ble_handle_gap_enc_change },
    { BLE_GAP_EVENT_IDENTITY_RESOLVED, lua_ble_handle_gap_identity_resolved },
    { BLE_GAP_EVENT_REPEAT_PAIRING, lua_ble_handle_gap_repeat_pairing },
    { BLE_GAP_EVENT_SUBSCRIBE, lua_ble_handle_gap_subscribe },
    { BLE_GAP_EVENT_MTU, lua_ble_handle_gap_mtu },
};

static int lua_ble_dispatch_gap_event(struct ble_gap_event *event)
{
    for (size_t i = 0; i < sizeof(s_gap_event_handlers) / sizeof(s_gap_event_handlers[0]); i++) {
        if (s_gap_event_handlers[i].type == event->type) {
            return s_gap_event_handlers[i].handler(event);
        }
    }
    return 0;
}

static esp_err_t lua_ble_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    return err;
}

static const char *lua_ble_controller_status_name(esp_bt_controller_status_t status)
{
    switch (status) {
    case ESP_BT_CONTROLLER_STATUS_IDLE:
        return "IDLE";
    case ESP_BT_CONTROLLER_STATUS_INITED:
        return "INITED";
    case ESP_BT_CONTROLLER_STATUS_ENABLED:
        return "ENABLED";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t lua_ble_controller_start(void)
{
    esp_err_t err;
    esp_bt_controller_status_t status = esp_bt_controller_get_status();

    ESP_LOGI(TAG, "BLE controller status before init: %s", lua_ble_controller_status_name(status));

    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_controller_inited_by_ble = true;
        status = esp_bt_controller_get_status();
        ESP_LOGI(TAG, "BLE controller status after init: %s", lua_ble_controller_status_name(status));
    } else if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
        ESP_LOGI(TAG, "BLE controller already initialized");
    } else if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGI(TAG, "BLE controller already enabled");
        ESP_LOGI(TAG, "BLE controller status after init: %s", lua_ble_controller_status_name(status));
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Unsupported BLE controller status before init: %d", status);
        return ESP_ERR_INVALID_STATE;
    }

    status = esp_bt_controller_get_status();
    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGI(TAG, "BLE controller already enabled");
        return ESP_OK;
    }
    if (status != ESP_BT_CONTROLLER_STATUS_INITED) {
        ESP_LOGE(TAG, "BLE controller cannot enable from status: %s", lua_ble_controller_status_name(status));
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return err;
    }
    s_controller_enabled_by_ble = true;

    status = esp_bt_controller_get_status();
    ESP_LOGI(TAG, "BLE controller status after enable: %s", lua_ble_controller_status_name(status));
    return ESP_OK;
}

static void lua_ble_controller_stop(void)
{
    esp_err_t err;
    esp_bt_controller_status_t status = esp_bt_controller_get_status();

    ESP_LOGI(TAG, "BLE controller status before deinit: %s", lua_ble_controller_status_name(status));

    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        if (!s_controller_enabled_by_ble) {
            ESP_LOGI(TAG, "Skip BLE controller disable: controller was not enabled by lua_module_ble");
            return;
        }

        err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_bt_controller_disable returned %s", esp_err_to_name(err));
            return;
        }
        s_controller_enabled_by_ble = false;
        status = esp_bt_controller_get_status();
        ESP_LOGI(TAG, "BLE controller status after disable: %s", lua_ble_controller_status_name(status));
    }

    if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
        if (!s_controller_inited_by_ble) {
            ESP_LOGI(TAG, "Skip BLE controller deinit: controller was not initialized by lua_module_ble");
            return;
        }

        err = esp_bt_controller_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_bt_controller_deinit returned %s", esp_err_to_name(err));
            return;
        }
        s_controller_inited_by_ble = false;
        status = esp_bt_controller_get_status();
        ESP_LOGI(TAG, "BLE controller status after deinit: %s", lua_ble_controller_status_name(status));
    }
}

static esp_err_t lua_ble_stack_start(void)
{
    esp_err_t err;
    int rc;

    if (s_stack_inited) {
        ESP_LOGI(TAG, "NimBLE host already running");
        return ESP_OK;
    }

    err = lua_ble_stack_prepare();
    if (err != ESP_OK) {
        return err;
    }

    s_external_reset_cb = ble_hs_cfg.reset_cb;
    s_external_sync_cb = ble_hs_cfg.sync_cb;
    s_external_gatts_register_cb = ble_hs_cfg.gatts_register_cb;
    s_external_gatts_register_arg = ble_hs_cfg.gatts_register_arg;
    ble_hs_cfg.reset_cb = lua_ble_chained_reset_cb;
    ble_hs_cfg.sync_cb = lua_ble_chained_sync_cb;
    ble_hs_cfg.gatts_register_cb = lua_ble_chained_gatts_register_cb;
    ble_hs_cfg.gatts_register_arg = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    ESP_LOGI(TAG, "SMP configured: bonding=1 sc=1 mitm=0 io_cap=NO_IO");

    if (!s_external_gatts_register_cb) {
        ble_svc_gap_init();
        ble_svc_gatt_init();
    }

#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed rc=%d", rc);
        return ESP_FAIL;
    }
#else
    ESP_LOGI(TAG, "BLE test GATT service disabled");
#endif

    rc = ble_svc_gap_device_name_set(LUA_BLE_DEFAULT_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_host_synced = false;
    nimble_port_freertos_init(lua_ble_host_task);
    s_host_task_started = true;

    if (xSemaphoreTake(s_host_sync_sem, pdMS_TO_TICKS(LUA_BLE_HOST_SYNC_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE host sync timeout");
        (void)lua_ble_stack_stop();
        return ESP_ERR_TIMEOUT;
    }

    s_stack_inited = true;
    return ESP_OK;
}

static esp_err_t lua_ble_stack_stop(void)
{
    esp_err_t err;

    if (!s_stack_inited && !s_nimble_inited &&
            !s_host_task_started && !s_controller_inited_by_ble && !s_controller_enabled_by_ble) {
        return ESP_OK;
    }

    if (s_advertising) {
        (void)ble_gap_adv_stop();
        s_advertising = false;
    }
    if (s_scanning) {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }

    if (s_host_task_started) {
        err = nimble_port_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nimble_port_stop returned %s", esp_err_to_name(err));
        } else {
            s_host_task_started = false;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_nimble_inited) {
        err = esp_nimble_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_nimble_deinit returned %s", esp_err_to_name(err));
        } else {
            s_nimble_inited = false;
        }
    }

    lua_ble_controller_stop();

    s_stack_inited = false;
    s_host_synced = false;
    s_advertising_requested = false;
    lua_ble_connection_clear();
    return ESP_OK;
}

static esp_err_t lua_ble_adv_apply(const char *name, uint16_t appearance, const uint16_t *uuid16_list,
                                   size_t uuid16_count)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    uint8_t adv_payload[BLE_HS_ADV_MAX_SZ];
    uint8_t adv_payload_len = 0;
    uint8_t empty_scan_rsp = 0;
    ble_uuid16_t adv_uuids[LUA_BLE_ADV_UUID16_MAX];
    int rc;
    size_t i;

    if (!s_stack_inited || !s_host_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uuid16_count > sizeof(adv_uuids) / sizeof(adv_uuids[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    if (appearance != 0) {
        fields.appearance = appearance;
        fields.appearance_is_present = 1;
    }
    if (uuid16_count > 0) {
        for (i = 0; i < uuid16_count; i++) {
            adv_uuids[i] = (ble_uuid16_t)BLE_UUID16_INIT(uuid16_list[i]);
            ESP_LOGI(TAG, "advertising uuid16[%u]=0x%04x", (unsigned int)i, uuid16_list[i]);
        }
        fields.uuids16 = adv_uuids;
        fields.num_uuids16 = uuid16_count;
        fields.uuids16_is_complete = 1;
    }
    ESP_LOGI(TAG, "advertising fields name=%s flags=0x%02x appearance=0x%04x uuid16_count=%u",
             name,
             fields.flags,
             appearance,
             (unsigned int)uuid16_count);
    rc = ble_hs_adv_set_fields(&fields, adv_payload, &adv_payload_len, sizeof(adv_payload));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_adv_set_fields failed rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "advertising payload len=%u", adv_payload_len);
    ESP_LOG_BUFFER_HEX(TAG, adv_payload, adv_payload_len);

    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gap_adv_rsp_set_data(&empty_scan_rsp, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_data empty failed rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "scan response payload empty");

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, lua_ble_gap_event, NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_advertising = true;
    return ESP_OK;
}

static esp_err_t lua_ble_scan_start_internal(void)
{
    struct ble_gap_disc_params disc_params;
    int rc;

    if (!s_stack_inited || !s_host_synced) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &disc_params,
                      lua_ble_gap_event, NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_scanning = true;
    return ESP_OK;
}

static int lua_ble_init(lua_State *L)
{
    esp_err_t err = lua_module_ble_init_stack();

    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_deinit(lua_State *L)
{
    esp_err_t err = lua_module_ble_deinit_stack();

    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_adv_start(lua_State *L)
{
    const char *name = LUA_BLE_DEFAULT_NAME;
    esp_err_t err;
    int rc;
#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
    uint16_t uuid16 = LUA_BLE_GAP_SVC_UUID;
#endif

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "name");
    if (lua_type(L, -1) == LUA_TSTRING) {
        name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    if (strlen(name) > LUA_BLE_ADV_NAME_MAX) {
        return luaL_error(L, "name length must be <= %d", LUA_BLE_ADV_NAME_MAX);
    }

    if (!s_stack_inited || !s_host_synced) {
        return luaL_error(L, "ble.init() must succeed before ble.adv_start()");
    }

    rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        return luaL_error(L, "ble_svc_gap_device_name_set failed rc=%d", rc);
    }

#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
    err = lua_ble_adv_apply(name, 0, &uuid16, 1);
#else
    err = lua_ble_adv_apply(name, 0, NULL, 0);
#endif
    if (err != ESP_OK) {
        return luaL_error(L, "ble.adv_start failed: %s", esp_err_to_name(err));
    }
    strlcpy(s_adv_name, name, sizeof(s_adv_name));
    s_adv_appearance = 0;
#if CONFIG_APP_CLAW_BLE_TEST_SERVICE
    s_adv_uuid16_list[0] = uuid16;
    s_adv_uuid16_count = 1;
#else
    s_adv_uuid16_count = 0;
#endif
    s_advertising_requested = true;

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_adv_stop(lua_State *L)
{
    esp_err_t err = lua_module_ble_adv_stop();

    if (err != ESP_OK) {
        return luaL_error(L, "ble.adv_stop failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_scan_start(lua_State *L)
{
    esp_err_t err;

    (void)L;

    err = lua_ble_scan_start_internal();
    if (err != ESP_OK) {
        return luaL_error(L, "ble.scan_start failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_scan_stop(lua_State *L)
{
    int rc;

    (void)L;

    if (!s_stack_inited) {
        lua_pushboolean(L, 1);
        return 1;
    }

    rc = ble_gap_disc_cancel();

    if (rc != 0 && rc != BLE_HS_EALREADY) {
        return luaL_error(L, "ble_gap_disc_cancel failed rc=%d", rc);
    }

    s_scanning = false;
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_status(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, s_stack_inited && s_host_synced);
    lua_setfield(L, -2, "initialized");
    lua_pushboolean(L, s_advertising);
    lua_setfield(L, -2, "advertising");
    lua_pushboolean(L, s_scanning);
    lua_setfield(L, -2, "scanning");
    return 1;
}

static int lua_ble_connection(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, s_conn.connected);
    lua_setfield(L, -2, "connected");
    lua_pushinteger(L, s_conn.conn_handle);
    lua_setfield(L, -2, "conn_handle");
    lua_pushboolean(L, s_conn.encrypted);
    lua_setfield(L, -2, "encrypted");
    lua_pushboolean(L, s_conn.bonded);
    lua_setfield(L, -2, "bonded");
    lua_pushboolean(L, s_conn.notify_subscribed);
    lua_setfield(L, -2, "notify_subscribed");
    return 1;
}

int luaopen_ble(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_ble_init);
    lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_ble_deinit);
    lua_setfield(L, -2, "deinit");
    lua_pushcfunction(L, lua_ble_adv_start);
    lua_setfield(L, -2, "adv_start");
    lua_pushcfunction(L, lua_ble_adv_stop);
    lua_setfield(L, -2, "adv_stop");
    lua_pushcfunction(L, lua_ble_scan_start);
    lua_setfield(L, -2, "scan_start");
    lua_pushcfunction(L, lua_ble_scan_stop);
    lua_setfield(L, -2, "scan_stop");
    lua_pushcfunction(L, lua_ble_status);
    lua_setfield(L, -2, "status");
    lua_pushcfunction(L, lua_ble_connection);
    lua_setfield(L, -2, "connection");
    return 1;
}

esp_err_t lua_module_ble_register(void)
{
    return cap_lua_register_module(LUA_MODULE_BLE_NAME, luaopen_ble);
}

esp_err_t lua_module_ble_init_stack(void)
{
    return lua_ble_stack_start();
}

esp_err_t lua_module_ble_prepare_stack(void)
{
    return lua_ble_stack_prepare();
}

esp_err_t lua_module_ble_deinit_stack(void)
{
    return lua_ble_stack_stop();
}

esp_err_t lua_module_ble_adv_start(const char *name, uint16_t appearance, const uint16_t *uuid16_list,
                                   size_t uuid16_count)
{
    esp_err_t err;
    int rc;

    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(name) > LUA_BLE_ADV_NAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed rc=%d", rc);
        return ESP_FAIL;
    }

    err = lua_ble_adv_apply(name, appearance, uuid16_list, uuid16_count);
    if (err != ESP_OK) {
        return err;
    }
    strlcpy(s_adv_name, name, sizeof(s_adv_name));
    s_adv_appearance = appearance;
    if (uuid16_count > 0) {
        memcpy(s_adv_uuid16_list, uuid16_list, uuid16_count * sizeof(s_adv_uuid16_list[0]));
    }
    s_adv_uuid16_count = uuid16_count;
    s_advertising_requested = true;
    return ESP_OK;
}

esp_err_t lua_module_ble_adv_stop(void)
{
    int rc;

    if (!s_stack_inited) {
        return ESP_OK;
    }

    rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_stop failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_advertising = false;
    s_advertising_requested = false;
    return ESP_OK;
}

void lua_module_ble_get_connection(lua_module_ble_connection_t *out)
{
    if (out) {
        *out = s_conn;
    }
}
