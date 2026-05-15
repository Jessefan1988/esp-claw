/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "lua.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool connected;
    uint16_t conn_handle;
    bool encrypted;
    bool bonded;
    bool notify_subscribed;
} lua_module_ble_connection_t;

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_ble(lua_State *L);
esp_err_t lua_module_ble_register(void);
esp_err_t lua_module_ble_prepare_stack(void);
esp_err_t lua_module_ble_init_stack(void);
esp_err_t lua_module_ble_deinit_stack(void);
esp_err_t lua_module_ble_adv_start(const char *name, uint16_t appearance, const uint16_t *uuid16_list,
                                   size_t uuid16_count);
esp_err_t lua_module_ble_adv_stop(void);
bool lua_module_ble_is_notify_subscribed(uint16_t attr_handle);
esp_err_t lua_module_ble_notify(uint16_t attr_handle, const void *data, size_t len);
void lua_module_ble_get_connection(lua_module_ble_connection_t *out);

#ifdef __cplusplus
}
#endif
