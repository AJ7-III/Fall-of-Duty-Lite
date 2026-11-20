#include "board.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace board {
namespace {
constexpr auto tag = "fod-usb";
constexpr bool isDualShock4(uint16_t vendor, uint16_t product) {
    return vendor == 0x054c && (product == 0x05c4 || product == 0x09cc);
}

struct Report {
    int64_t us = 0;
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    bool present = false;
    bool sameControls(const Report& next) const {
        return buttons == next.buttons && lx == next.lx && ly == next.ly &&
               rx == next.rx && ry == next.ry && present == next.present;
    }
};

portMUX_TYPE reportLock = portMUX_INITIALIZER_UNLOCKED;
std::array<Report, 128> reports{};
Report latest;
uint32_t produced, consumed, overflows;
bool reportLost;

void enqueue(Report report) {
    portENTER_CRITICAL(&reportLock);
    if (!report.sameControls(latest)) {
        if (produced - consumed == reports.size()) {
            consumed = produced;
            ++overflows;
            reportLost = true;
        }
        latest = report;
        reports[produced++ % reports.size()] = report;
    }
    portEXIT_CRITICAL(&reportLock);
}

struct Device {
    usb_host_client_handle_t client = nullptr;
    usb_device_handle_t handle = nullptr;
    usb_transfer_t* transfer = nullptr;
    uint8_t newAddress = 0, endpoint = 0;
    bool disconnect = false, inFlight = false, claimed = false, buttonLog = false;
    bool firstReport = true, fireState = false;
    uint32_t requireRelease = 0;
};

void markDisconnected(Device& device) {
    device.disconnect = true;
    enqueue({.us = timeUs()});
#if CONFIG_FOD_LATENCY_GPIO >= 0
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_FOD_LATENCY_GPIO), 0);
    device.fireState = false;
#endif
}

void onTransfer(usb_transfer_t* transfer) {
    auto& device = *static_cast<Device*>(transfer->context);
    device.inFlight = false;
    if (device.disconnect) return;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        markDisconnected(device);
        return;
    }
    const uint8_t* data = transfer->data_buffer;
    if (transfer->actual_num_bytes >= 10 && data[0] == 0x01) {
        Report report{.us = timeUs(), .lx = data[1], .ly = data[2], .rx = data[3], .ry = data[4], .present = true};
        const uint8_t face = data[5], shoulders = data[6];
        const uint8_t dpad = face & 15;
        if (face & 0x10) report.buttons |= fod::Reload;
        if (face & 0x20) report.buttons |= fod::Jump | fod::Confirm;
        if (face & 0x40) report.buttons |= fod::Stance | fod::Back;
        if (face & 0x80) report.buttons |= fod::Swap;
        if ((shoulders & 0x04) || data[8] >= 48) report.buttons |= fod::Aim;
        if ((shoulders & 0x08) || data[9] >= 48) report.buttons |= fod::Fire;
        if (shoulders & 0x01) report.buttons |= fod::Signal;
        if (shoulders & 0x20) report.buttons |= fod::Pause;
        if (shoulders & 0x40) report.buttons |= fod::Sprint;
        if (dpad == 0 || dpad == 1 || dpad == 7) report.buttons |= fod::Up | fod::Streak;
        if (dpad >= 3 && dpad <= 5) report.buttons |= fod::Down;
        if (dpad >= 5 && dpad <= 7) report.buttons |= fod::Left;
        if (dpad >= 1 && dpad <= 3) report.buttons |= fod::Right;

        if (device.firstReport) {
            device.requireRelease = report.buttons;
            device.firstReport = false;
        }
        device.requireRelease &= report.buttons;
        report.buttons &= ~device.requireRelease;
#if CONFIG_FOD_LATENCY_GPIO >= 0
        const bool fire = (report.buttons & fod::Fire) != 0;
        if (fire != device.fireState) gpio_set_level(static_cast<gpio_num_t>(CONFIG_FOD_LATENCY_GPIO), fire);
        device.fireState = fire;
#endif
        enqueue(report);
        if (!device.buttonLog) {
            ESP_LOGI(tag, "USB HID 0x01 packet size=%d buttons=%02x/%02x", transfer->actual_num_bytes,
                     face, shoulders);
            device.buttonLog = true;
        }
    }
    device.inFlight = usb_host_transfer_submit(transfer) == ESP_OK;
    if (!device.inFlight) markDisconnected(device);
}

void onDeviceEvent(const usb_host_client_event_msg_t* msg, void* context) {
    auto& device = *static_cast<Device*>(context);
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) device.newAddress = msg->new_dev.address;
    if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && msg->dev_gone.dev_hdl == device.handle) {
        markDisconnected(device);
    }
}

esp_err_t openDevice(Device& d, uint8_t address) {
    if (const auto err = usb_host_device_open(d.client, address, &d.handle); err != ESP_OK) return err;
    const usb_device_desc_t* descriptor;
    if (const auto err = usb_host_get_device_descriptor(d.handle, &descriptor); err != ESP_OK) return err;
    ESP_LOGI(tag, "device %04x:%04x at address %u", descriptor->idVendor, descriptor->idProduct, address);
    if (!isDualShock4(descriptor->idVendor, descriptor->idProduct)) return ESP_ERR_NOT_SUPPORTED;

    const usb_config_desc_t* config;
    if (const auto err = usb_host_get_active_config_descriptor(d.handle, &config); err != ESP_OK) return err;
    int offset = 0;
    const auto* interface = usb_parse_interface_descriptor(config, 0, 0, &offset);
    if (!interface || interface->bInterfaceClass != USB_CLASS_HID) return ESP_ERR_NOT_SUPPORTED;
    const usb_ep_desc_t* ep = nullptr;
    for (int i = 0; i < interface->bNumEndpoints; ++i) {
        int endpointOffset = offset;
        const auto* candidate = usb_parse_endpoint_descriptor_by_index(interface, i, config->wTotalLength, &endpointOffset);
        if (candidate && (candidate->bEndpointAddress & 0x80) && (candidate->bmAttributes & 3) == USB_BM_ATTRIBUTES_XFER_INT) {
            ep = candidate;
            break;
        }
    }
    if (!ep) return ESP_ERR_NOT_SUPPORTED;
    const int bytes = ep->wMaxPacketSize & 0x7ff;
    if (bytes < 10 || bytes > 64) return ESP_ERR_NOT_SUPPORTED;
    if (const auto err = usb_host_interface_claim(d.client, d.handle, 0, 0); err != ESP_OK) return err;
    d.claimed = true;
    d.endpoint = ep->bEndpointAddress;
    if (const auto err = usb_host_transfer_alloc(bytes, 0, &d.transfer); err != ESP_OK) return err;
    d.transfer->device_handle = d.handle;
    d.transfer->bEndpointAddress = d.endpoint;
    d.transfer->callback = onTransfer;
    d.transfer->context = &d;
    d.transfer->num_bytes = bytes;
    d.firstReport = true;
    d.buttonLog = false;
    d.fireState = false;
    if (const auto err = usb_host_transfer_submit(d.transfer); err != ESP_OK) return err;
    d.inFlight = true;
    ESP_LOGI(tag, "DualShock 4 interrupt-IN endpoint 0x%02x (%d bytes)", d.endpoint, bytes);
    return ESP_OK;
}

void closeDevice(Device& d) {
    if (d.inFlight) {
        usb_host_endpoint_halt(d.handle, d.endpoint);
        usb_host_endpoint_flush(d.handle, d.endpoint);
        return; // The completion callback hands the transfer back to this task.
    }
    if (d.transfer) usb_host_transfer_free(d.transfer);
    if (d.claimed) usb_host_interface_release(d.client, d.handle, 0);
    if (d.handle) usb_host_device_close(d.client, d.handle);
    d.transfer = nullptr;
    d.handle = nullptr;
    d.claimed = false;
    d.disconnect = false;
}

void gamepadTask(void*) {
    Device device;
    const usb_host_client_config_t config{
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {.client_event_callback = onDeviceEvent, .callback_arg = &device},
    };
    if (usb_host_client_register(&config, &device.client) != ESP_OK) {
        ESP_LOGE(tag, "client registration failed");
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        usb_host_client_handle_events(device.client, portMAX_DELAY);
        if (device.disconnect) closeDevice(device);
        if (!device.handle && device.newAddress) {
            const auto address = std::exchange(device.newAddress, 0);
            const auto err = openDevice(device, address);
            if (err != ESP_OK) {
                ESP_LOGW(tag, "controller open failed (%s)", esp_err_to_name(err));
                closeDevice(device);
            }
        }
    }
}

void usbTask(void*) {
    while (true) {
        uint32_t events = 0;
        if (usb_host_lib_handle_events(portMAX_DELAY, &events) == ESP_OK &&
            (events & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) {
            usb_host_device_free_all();
        }
    }
}

std::pair<float, float> stick(uint8_t rawX, uint8_t rawY, float deadZone, bool look) {
    const float x = (static_cast<float>(rawX) - 128.0f) / 127.0f;
    const float y = (static_cast<float>(rawY) - 128.0f) / 127.0f;
    const float length = std::hypot(x, y);
    if (length <= deadZone) return {};
    const float unit = std::min((length - deadZone) / (1.0f - deadZone), 1.0f);
    const float magnitude = look ? (0.42f * unit + 0.58f * unit * unit * unit) : unit;
    return {x * magnitude / length, y * magnitude / length};
}
} // namespace

esp_err_t startGamepad() {
#if CONFIG_FOD_LATENCY_GPIO >= 0
    const auto marker = static_cast<gpio_num_t>(CONFIG_FOD_LATENCY_GPIO);
    if (!GPIO_IS_VALID_OUTPUT_GPIO(marker)) return ESP_ERR_INVALID_ARG;
    gpio_reset_pin(marker);
    gpio_set_direction(marker, GPIO_MODE_OUTPUT);
    gpio_set_level(marker, 0);
#endif
    const usb_host_config_t config{.intr_flags = ESP_INTR_FLAG_LEVEL1};
    if (const auto err = usb_host_install(&config); err != ESP_OK) return err;
    if (xTaskCreatePinnedToCore(gamepadTask, "fod-pad", 4096, nullptr, 12, nullptr, 0) != pdPASS ||
        xTaskCreatePinnedToCore(usbTask, "fod-usb", 4096, nullptr, 13, nullptr, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

fod::Input controllerAt(int64_t deadline) {
    static Report applied;
    uint32_t edge = 0;
    bool disconnected = false;
    portENTER_CRITICAL(&reportLock);
    if (reportLost) {
        disconnected = true;
        reportLost = false;
        applied = {};
    }
    while (consumed != produced) {
        const auto& report = reports[consumed % reports.size()];
        if (report.us > deadline) break;
        disconnected |= applied.present && !report.present;
        edge = report.present ? edge | (report.buttons & ~applied.buttons) : 0;
        applied = report;
        ++consumed;
    }
    const Report report = applied;
    portEXIT_CRITICAL(&reportLock);
    if (!report.present || disconnected) {
        // A disconnect wins over pending button presses on the same tick.
        return {.disconnected = true};
    }
    const auto [mx, my] = stick(report.lx, report.ly, 0.14f, false);
    const auto [rx, ry] = stick(report.rx, report.ry, 0.12f, true);
    return {mx, -my, rx * (3.3f / 60.0f), ry * (2.5f / 60.0f), report.buttons, edge};
}

bool hasController() {
    portENTER_CRITICAL(&reportLock);
    const bool present = latest.present;
    portEXIT_CRITICAL(&reportLock);
    return present;
}

uint32_t reportOverflows() {
    portENTER_CRITICAL(&reportLock);
    const auto value = overflows;
    portEXIT_CRITICAL(&reportLock);
    return value;
}

} // namespace board
