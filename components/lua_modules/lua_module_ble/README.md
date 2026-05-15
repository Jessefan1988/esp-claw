# Lua BLE

## Overview

`lua_module_ble` exposes a minimal NimBLE-based BLE stack to Lua on ESP-Claw: controller and host bring-up, connectable legacy advertising, passive/active discovery scan logging, and a small custom GATT service for bring-up validation. The implementation maps directly to ESP-IDF NimBLE and `esp_bt` APIs without an intermediate framework.

## Current capabilities

- Controller: status-aware bring-up via `esp_bt_controller_get_status`, `esp_bt_controller_init`, and `esp_bt_controller_enable(ESP_BT_MODE_BLE)`.
- Host: `esp_nimble_init`, `nimble_port_freertos_init(lua_ble_host_task)`, host sync wait, `nimble_port_stop`, and `esp_nimble_deinit`.
- Connection state: connected flag, connection handle, encryption state, bonding state, and notification subscription state for generic GATT use.
- Security: NimBLE SMP bonding with Secure Connections and no-input/no-output IO capability.
- Advertising: legacy ADV, general discoverable, BR/EDR not supported flag,
  complete local name, and 16-bit service UUID `0xFFF0` in the AD payload when
  `CONFIG_APP_CLAW_BLE_TEST_SERVICE=y`.
- Scanning: `ble_gap_disc` with duplicate filtering and `passive = 0` (active scan); each report logged with address, RSSI, and device name when present in AD (`ESP_LOGI`, tag `lua_ble`)
- Optional test GATT server: primary service `0xFFF0`, characteristic `0xFFF1`
  with READ, WRITE, NOTIFY; default value `hello` (writes persist until
  overwritten or `deinit`).

## Prerequisites

1. **sdkconfig** (menuconfig): enable *Component config → Bluetooth → Bluetooth*, choose **NimBLE** host (not Bluedroid). Typical symbols: `CONFIG_BT_ENABLED`, `CONFIG_BT_NIMBLE_ENABLED`, Bluedroid disabled.
2. **Kconfig**: `App Claw Config → App Claw Lua Modules → Enable BLE Lua module` (`CONFIG_APP_CLAW_LUA_MODULE_BLE`).
3. **NVS**: `ble.init()` runs `nvs_flash_init()` using the same rules as ESP-IDF BLE examples (erase + re-init on `NO_FREE_PAGES` / `NEW_VERSION_FOUND`).

`CONFIG_APP_CLAW_BLE_TEST_SERVICE` defaults to `y` when
`CONFIG_APP_CLAW_LUA_MODULE_BLE=y`. Keep it enabled for nRF Connect bring-up
testing; disable it for HID-focused builds that should not expose the
`0xFFF0/0xFFF1` test service.

## Lua API

| Function | Description |
|----------|-------------|
| `ble.init()` | Start the BLE controller if needed, initialize the NimBLE host, register GAP/GATT, and wait for host sync. Repeated calls are idempotent. Returns `true` or `nil, err`. |
| `ble.deinit()` | Stop advertising/scan if active, stop/deinitialize the NimBLE host, and release only controller state owned by this module. Repeated calls are idempotent. Returns `true` or `nil, err`. |
| `ble.adv_start({ name = "esp-claw" })` | Set GAP device name and start undirected connectable advertising. `name` optional; length ≤ 29 octets. |
| `ble.adv_stop()` | Stop advertising. |
| `ble.scan_start()` | Start general discovery (`ble_gap_disc`, active scan). |
| `ble.scan_stop()` | Cancel discovery. |
| `ble.status()` | Table: `initialized`, `advertising`, `scanning` (booleans). |
| `ble.connection()` | Table: `connected`, `conn_handle`, `encrypted`, `bonded`, `notify_subscribed`. |

## Initialization flow (Lua)

```lua
local ble = require("ble")
assert(ble.init())
-- stack is up; GATT DB registered; default GAP name "esp-claw" until adv_start changes it
```

Sequence on device: NVS → read `esp_bt_controller_get_status()` → initialize/enable the controller only when needed → `esp_nimble_init` → GAP/GATT service init → `ble_gatts_count_cfg` / `ble_gatts_add_svcs` → default device name → `nimble_port_freertos_init` → wait for `ble_hs_cfg.sync_cb`.

Controller handling:

- `IDLE`: initialize and enable the controller.
- `INITED`: enable the controller.
- `ENABLED`: skip controller init/enable and continue with NimBLE host setup.

`ble.deinit()` reverses only the resources started by `lua_module_ble`; it does not deinitialize a controller that was initialized or enabled by another component.

## Connection and Security

`lua_module_ble` tracks one active connection for the current minimal GATT server. GAP events update the state:

- `CONNECT`: sets `connected` and `conn_handle`.
- `DISCONNECT`: clears connection state and restarts advertising if advertising was requested.
- `SUBSCRIBE`: updates `notify_subscribed` for the active connection. This state
  is used by the optional BLE test characteristic and generic GATT helpers.
- `ENC_CHANGE` and `IDENTITY_RESOLVED`: refresh `encrypted` and `bonded`.
- `MTU` and `ADV_COMPLETE`: logged for bring-up and later HID diagnostics.

SMP is configured with bonding enabled, Secure Connections enabled, MITM disabled, and no-input/no-output IO capability. Key distribution includes encryption and identity keys.

Notifications are sent through an internal helper that checks connection and CCC
subscription state before calling NimBLE notification APIs. The helper is used
by the optional BLE test characteristic and generic GATT helpers; it is not
exposed as a Lua API.

## Advertising example

```lua
local ble = require("ble")
ble.init()
ble.adv_start({ name = "esp-claw" })
print(ble.status().advertising)
ble.adv_stop()
```

## Scanning example

```lua
local ble = require("ble")
ble.init()
ble.scan_start()
-- Watch serial log (tag lua_ble) for MAC, RSSI, name
ble.scan_stop()
ble.deinit()
```

## GATT example

When `CONFIG_APP_CLAW_BLE_TEST_SERVICE=y`, after `ble.init()` and optional
`ble.adv_start`, use a phone GATT client (e.g. nRF Connect):

- Connect to the device.
- Locate service **FFF0**, characteristic **FFF1**.
- Read: expect `hello` until a write changes the value.
- Write: new value is stored and a notification is sent via the internal notify
  helper when a peer is connected and has enabled notifications.

## Quick Test

Use `test/test_ble.lua` after building and flashing `edge_agent` with NimBLE and `APP_CLAW_LUA_MODULE_BLE` enabled. During the `edge_agent` build, module test scripts are synced into the FATFS builtin Lua script area.

From the `app>` console, list and run the synced test script:

```text
lua --list --keyword test_ble
lua --run --path builtin/test/test_ble.lua --timeout-ms 30000
```

The script performs:

- `require("ble")`
- `ble.init()`
- `ble.connection()`
- status print
- `ble.adv_start({ name = "esp-claw" })`
- a short advertising hold
- `ble.scan_start()`
- a short scan window
- `ble.scan_stop()`
- `ble.adv_stop()`
- `ble.deinit()`

Expected serial output includes lines similar to:

```text
[ble_test] require ble -> table: 0x...
[ble_test] step: ble.init
I (...) lua_ble: BLE controller status before init: IDLE
I (...) lua_ble: BLE controller status after init: INITED
I (...) lua_ble: BLE controller status after enable: ENABLED
[ble_test] ble.init -> ok=true result=true err=nil
[ble_test] status after init -> ok=true result={initialized=true, advertising=false, scanning=false} err=nil
[ble_test] connection after ble.init -> ok=true result={connected=false, conn_handle=65535, encrypted=false, bonded=false, notify_subscribed=false} err=nil
[ble_test] step: ble.adv_start
[ble_test] ble.adv_start -> ok=true result=true err=nil
[ble_test] status while advertising -> ok=true result={initialized=true, advertising=true, scanning=false} err=nil
[ble_test] step: ble.scan_start
I (...) lua_ble: scan mac=... rssi=... name=...
[ble_test] step: ble.deinit
[ble_test] done
```

To verify advertising with nRF Connect:

- Open nRF Connect while the script is in the advertising hold.
- Scan for device name `esp-claw`.
- Connect to the device.
- Confirm service UUID `0xFFF0`.
- Confirm characteristic UUID `0xFFF1`.

If `CONFIG_APP_CLAW_BLE_TEST_SERVICE=n`, the same script still validates BLE
stack bring-up, advertising, scanning, and shutdown, but the `0xFFF0/0xFFF1`
GATT service is intentionally absent.

## Limitations

- No manufacturer-specific data, beacons, or extended advertising.
- The `0xFFF0/0xFFF1` development GATT service is controlled by
  `CONFIG_APP_CLAW_BLE_TEST_SERVICE`.
- SMP bonding and Secure Connections are configured for dependent HID use. Pairing
  key persistence still depends on the project NimBLE NVS settings, for example
  `CONFIG_BT_NIMBLE_NVS_PERSIST`.
- `ble.deinit()` is idempotent and releases only resources owned by `lua_module_ble`.
- Concurrent advertising and scanning depends on controller capabilities; failures surface as Lua errors from NimBLE return codes.
- BLE HID is handled by `lua_module_ble_hid`, which owns its own NimBLE HID
  runtime. Do not start this generic BLE module's advertising while BLE HID is
  running.
- If the target behavior is keyboard, mouse, or media control, use
  `lua_module_ble_hid` and its `ble_hid_skills/scripts/start_ble_hid.lua`
  runtime entry point instead of `test/test_ble.lua`.

## Roadmap (not implemented)

- Richer GATT profiles and application-defined services
- Manufacturer data / extended advertising
- Optional bonding and NVS-backed store configuration

## Verification

| Goal | Steps |
|------|--------|
| Phone scan / name | Build with NimBLE + `APP_CLAW_LUA_MODULE_BLE`, flash, run `ble.init()` then `ble.adv_start({ name = "esp-claw" })`. On the phone, open a BLE scanner; confirm the local name and connectability. |
| Advertising | `ble.status().advertising == true` after `adv_start`; `false` after `adv_stop` or `deinit`. |
| GATT read/write/notify | Connect with nRF Connect; read/write FFF1; enable notifications on CCC after a write to observe updates. |
| Scan logs | `ble.scan_start()`; confirm `ESP_LOGI` lines with MAC, RSSI, and name when AD includes a name. |
| Lua API | From the `app>` console, run `lua --run --path builtin/test/test_ble.lua --timeout-ms 30000`; check return values and `ble.status()`. |
