if(CONFIG_APP_CLAW_LUA_MODULE_BLE)
    if(NOT CONFIG_BT_ENABLED OR NOT CONFIG_BT_NIMBLE_ENABLED)
        message(FATAL_ERROR
            "APP_CLAW_LUA_MODULE_BLE requires ESP-IDF Bluetooth with NimBLE host. "
            "Enable CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED."
        )
    endif()

    set(lua_module_ble_srcs "src/lua_module_ble.c")
endif()

idf_component_register(
    SRCS
        ${lua_module_ble_srcs}
    INCLUDE_DIRS
        "src"
    REQUIRES
        bt
        cap_lua
        freertos
        georgik__lua
        nvs_flash
)
