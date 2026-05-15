-- Minimal BLE bring-up test.
-- Verifies init, status, advertising, scanning, stop, and deinit paths.

local LOG_PREFIX = "[ble_test]"
local ADV_NAME = "esp-claw"
local ADV_HOLD_MS = 5000
local SCAN_HOLD_MS = 5000

local delay_ok, delay = pcall(require, "delay")
local ble_module = nil

local function sleep_ms(ms)
    if delay_ok and delay and type(delay.delay_ms) == "function" then
        delay.delay_ms(ms)
        return
    end

    if os and type(os.clock) == "function" then
        local deadline = os.clock() + (ms / 1000)
        while os.clock() < deadline do
        end
        return
    end

    -- Last-resort fallback for very small Lua runtimes without delay or os.clock.
    for _ = 1, ms * 1000 do
    end
end

local function table_to_string(value)
    if type(value) ~= "table" then
        return tostring(value)
    end

    local parts = {}
    for key, item in pairs(value) do
        parts[#parts + 1] = tostring(key) .. "=" .. tostring(item)
    end
    return "{" .. table.concat(parts, ", ") .. "}"
end

local function log(message)
    print(LOG_PREFIX .. " " .. message)
end

local function log_result(step, ok, result, err)
    log(step .. " -> ok=" .. tostring(ok)
        .. " result=" .. table_to_string(result)
        .. " err=" .. tostring(err))
end

local function print_status(ble, label)
    local ok, status = pcall(ble.status)
    log_result("status " .. label, ok, status, nil)
    if not ok then
        error(status)
    end
    return status
end

local function print_connection(ble, label)
    local ok, connection = pcall(ble.connection)
    log_result("connection " .. label, ok, connection, nil)
    if not ok then
        error(connection)
    end
    return connection
end

local function call_and_status(ble, step, fn)
    log("step: " .. step)
    local ok, result, err = pcall(fn)
    log_result(step, ok, result, err)
    print_status(ble, "after " .. step)
    print_connection(ble, "after " .. step)
    if not ok then
        error(result)
    end
    if result == nil then
        error(step .. " failed: " .. tostring(err))
    end
    return result, err
end

local function run()
    log("require ble")
    local ble = require("ble")
    ble_module = ble
    log("require ble -> " .. tostring(ble))

    call_and_status(ble, "ble.init", function()
        return ble.init()
    end)

    call_and_status(ble, "ble.adv_start", function()
        return ble.adv_start({
            name = ADV_NAME,
        })
    end)

    log("advertising for " .. tostring(ADV_HOLD_MS) .. " ms; scan with nRF Connect for name " .. ADV_NAME)
    sleep_ms(ADV_HOLD_MS)
    print_status(ble, "while advertising")
    print_connection(ble, "while advertising")

    call_and_status(ble, "ble.scan_start", function()
        return ble.scan_start()
    end)

    log("scanning for " .. tostring(SCAN_HOLD_MS) .. " ms; watch serial log tag lua_ble")
    sleep_ms(SCAN_HOLD_MS)

    call_and_status(ble, "ble.scan_stop", function()
        return ble.scan_stop()
    end)

    call_and_status(ble, "ble.adv_stop", function()
        return ble.adv_stop()
    end)

    call_and_status(ble, "ble.deinit", function()
        return ble.deinit()
    end)

    log("done")
end

local function cleanup()
    if ble_module then
        pcall(ble_module.scan_stop)
        pcall(ble_module.adv_stop)
        pcall(ble_module.deinit)
        ble_module = nil
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    log("ERROR: " .. tostring(err))
    error(err)
end
