#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "mcp_server.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "esp_video.h"
#include "sd_card_manager.h"
#include "sd_music_player.h"
#include "media_transfer_server.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_timer.h>
#include "esp_idf_version.h"
#include <cinttypes>
#include <algorithm>
#include <atomic>
#include <memory>

#include <driver/i2c_master.h>
#include <cstdlib>
#include "i2c_device.h"
#include "i2c_bus.h"
#include "bmi270_api.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include "esp_lcd_touch_cst816s.h"
#include "touch.h"

extern "C" {
#include "touch_button_sensor.h"
}

#include "driver/temperature_sensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "ESP-VoCat"

namespace Bmi270Motion {
static bmi270_handle_t bmi_handle_ = nullptr;

esp_err_t Initialize(i2c_bus_handle_t i2c_bus)
{
    if (bmi_handle_) {
        return ESP_OK;
    }
    if (!i2c_bus) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = bmi270_sensor_create(i2c_bus, &bmi_handle_, bmi270_config_file,
                                         BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (ret != ESP_OK || !bmi_handle_) {
        ESP_LOGW(TAG, "BMI270 init failed: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    const uint8_t sens_list[] = {BMI2_ACCEL};
    int8_t rslt = bmi270_sensor_enable(sens_list, 1, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGW(TAG, "BMI270 accel enable failed: %d", rslt);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BMI270 initialized");
    return ESP_OK;
}

bool ReadAccelRaw(struct bmi2_sens_data& accel)
{
    if (!bmi_handle_) {
        return false;
    }
    int8_t rslt = bmi2_get_sensor_data(&accel, bmi_handle_);
    return rslt == BMI2_OK;
}
} // namespace Bmi270Motion


temperature_sensor_handle_t temp_sensor = NULL;
static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};
float tsens_value;
gpio_num_t AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_1;
gpio_num_t AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_1;
gpio_num_t QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_1;
gpio_num_t TOUCH_PAD2 = TOUCH_PAD2_1;
gpio_num_t UART1_TX = UART1_TX_1;
gpio_num_t UART1_RX = UART1_RX_1;

class EspVocat;

class Charge : public I2cDevice {
public:
    struct BatteryInfo {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        int voltage_mv = 0;
        int current_ma = 0;
    };

    static constexpr uint8_t kRegVoltage = 0x08;
    static constexpr uint8_t kRegBatteryStatus = 0x0A;
    static constexpr uint8_t kRegCurrent = 0x0C;
    static constexpr uint8_t kRegStateOfCharge = 0x2C;
    static constexpr uint8_t kRegAverageCurrent = 0x14;
    static constexpr int kChargingCurrentMa = 30;
    static constexpr uint16_t kBatteryStatusDsg = BIT0;

    Charge(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {}

    bool GetBatteryInfo(BatteryInfo& info)
    {
        taskENTER_CRITICAL(&data_lock_);
        const bool valid = data_valid_;
        info = battery_info_;
        taskEXIT_CRITICAL(&data_lock_);
        return valid;
    }

    bool Update()
    {
        int16_t voltage = 0;
        int16_t current = 0;
        int16_t state_of_charge = 0;
        int16_t status = 0;
        int16_t average_current = 0;
        if (!TryReadWord(kRegVoltage, voltage, "voltage") ||
            !TryReadWord(kRegCurrent, current, "current") ||
            !TryReadWord(kRegStateOfCharge, state_of_charge, "state of charge") ||
            !TryReadWord(kRegBatteryStatus, status, "battery status") ||
            !TryReadWord(kRegAverageCurrent, average_current, "average current")) {
            return false;
        }

        BatteryInfo info;
        info.level = state_of_charge;
        if (info.level < 0) {
            info.level = 0;
        } else if (info.level > 100) {
            info.level = 100;
        }
        info.voltage_mv = static_cast<uint16_t>(voltage);
        info.current_ma = current;
        info.discharging = (static_cast<uint16_t>(status) & kBatteryStatusDsg) != 0;
        info.charging = !info.discharging || average_current > kChargingCurrentMa || current > kChargingCurrentMa;

        taskENTER_CRITICAL(&data_lock_);
        battery_info_ = info;
        data_valid_ = true;
        taskEXIT_CRITICAL(&data_lock_);

        const esp_err_t temperature_ret = temperature_sensor_get_celsius(temp_sensor, &tsens_value);
        if (temperature_ret != ESP_OK) {
            ++temperature_fail_count_;
            if (temperature_fail_count_ == 1 || (temperature_fail_count_ % 30) == 0) {
                ESP_LOGW(TAG, "Temperature read failed: %s (fail_count=%lu)",
                         esp_err_to_name(temperature_ret), static_cast<unsigned long>(temperature_fail_count_));
            }
        } else {
            temperature_fail_count_ = 0;
        }

        if (read_fail_count_ > 0) {
            ESP_LOGI(TAG, "Charge I2C recovered after %lu failures", static_cast<unsigned long>(read_fail_count_));
            read_fail_count_ = 0;
        }
        ESP_LOGD(TAG, "Battery: voltage=%dmV, current=%dmA, SOC=%d%%",
                 info.voltage_mv, info.current_ma, info.level);
        return true;
    }

private:
    bool TryReadWord(uint8_t reg, int16_t& value, const char* field)
    {
        uint8_t data[2] = {0};
        const esp_err_t ret = TryReadRegs(reg, data, sizeof(data));
        if (ret != ESP_OK) {
            ++read_fail_count_;
            if (read_fail_count_ == 1 || (read_fail_count_ % 30) == 0) {
                ESP_LOGW(TAG, "Charge read failed (%s, reg=0x%02X): %s (fail_count=%lu)",
                         field, reg, esp_err_to_name(ret), static_cast<unsigned long>(read_fail_count_));
            }
            return false;
        }
        value = static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                     (static_cast<uint16_t>(data[1]) << 8));
        return true;
    }

    portMUX_TYPE data_lock_ = portMUX_INITIALIZER_UNLOCKED;
    BatteryInfo battery_info_;
    bool data_valid_ = false;
    uint32_t read_fail_count_ = 0;
    uint32_t temperature_fail_count_ = 0;
};

class EspVocat : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_bus_handle_t shared_i2c_bus_handle_ = nullptr;
    Charge* charge_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    PwmBacklight* backlight_ = nullptr;
    esp_timer_handle_t touchpad_timer_;
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t tp_ = nullptr;
    EspVideo* camera_ = nullptr;
    TaskHandle_t charge_task_handle_ = nullptr;
    TaskHandle_t touch_task_handle_ = nullptr;
    TaskHandle_t imu_task_handle_ = nullptr;
    TaskHandle_t touch_slider_task_handle_ = nullptr;
    esp_timer_handle_t emotion_reset_timer_ = nullptr;
    bool bmi270_ready_ = false;
    bool was_charging_ = false;
    uint8_t low_battery_alert_mask_ = 0;
    int low_battery_plays_left_ = 0;
    int64_t next_low_battery_play_ms_ = 0;
    touch_button_handle_t touch_button_handle_ = nullptr;
    bool two_pad_touch_mode_ = false;
    uint32_t touch_pad1_channel_ = 0;
    uint32_t touch_pad2_channel_ = 0;
    uint8_t touch_active_mask_ = 0;
    uint32_t touch_start_channel_ = 0;
    int64_t touch_start_time_us_ = 0;
    bool touch_release_pending_ = false;
    int64_t touch_release_time_us_ = 0;
    bool touch_swipe_detected_ = false;
    std::unique_ptr<SdMusicPlayer> music_player_;
    std::unique_ptr<MediaTransferServer> media_transfer_;

    enum class TouchPage {
        kHidden,
        kPulling,
        kMenu,
        kVolume,
        kBrightness,
        kWallpaper,
        kMusic,
        kMusicPlayback,
    };
    TouchPage touch_page_ = TouchPage::kHidden;
    bool screen_touch_down_ = false;
    std::atomic<bool> touch_ui_busy_{false};
    bool suppress_screen_release_ = false;
    bool slider_dragging_ = false;
    int touch_start_x_ = 0;
    int touch_start_y_ = 0;
    int touch_last_x_ = 0;
    int touch_last_y_ = 0;
    int64_t screen_touch_start_us_ = 0;
    int touch_value_ = 0;
    int wallpaper_candidate_ = 0;
    int music_first_index_ = 0;
    int music_selected_index_ = -1;
    int64_t music_ui_refresh_us_ = 0;
    int64_t touch_render_us_ = 0;
    uint32_t music_library_revision_ = 0;
    uint32_t music_state_revision_ = 0;
    int rendered_music_first_index_ = -1;
    int rendered_music_selected_index_ = -2;
    std::array<std::string, 4> touch_menu_status_;

    static void emotion_reset_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self && self->display_ != nullptr) {
            self->display_->SetEmotion("neutral");
        }
    }

    void ShowTemporaryEmotion(const char* emotion, uint32_t duration_ms)
    {
        if (display_ == nullptr || emotion == nullptr) {
            return;
        }
        display_->SetEmotion(emotion);
        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
            esp_timer_start_once(emotion_reset_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL);
        }
    }

    void ShowTouchFeedback(const char* emotion)
    {
        static int64_t s_last_us = 0;
        constexpr int64_t kCooldownUs = 1200000;
        const int64_t now = esp_timer_get_time();
        if ((now - s_last_us) < kCooldownUs) {
            return;
        }
        s_last_us = now;
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display_);
        if (emote_display != nullptr && emote_display->InsertAnimDialog(emotion, 2000)) {
            if (emotion_reset_timer_ != nullptr) {
                esp_timer_stop(emotion_reset_timer_);
            }
            return;
        }
#endif
        ShowTemporaryEmotion(emotion, 2000);
    }

    void PlayBatteryEmotion(const char* emotion, uint32_t duration_ms)
    {
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        if (display_ == nullptr || emotion == nullptr) {
            return;
        }
        auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display_);
        if (emote_display != nullptr) {
            emote_display->InsertAnimDialog(emotion, duration_ms);
            return;
        }
#endif
        ShowTemporaryEmotion(emotion, duration_ms);
    }

    void StartLowBatteryAlertSequence()
    {
        constexpr int kMaxPlays = 3;
        constexpr int64_t kIntervalMs = 5 * 60 * 1000;

        low_battery_plays_left_ = kMaxPlays - 1;
        next_low_battery_play_ms_ = (esp_timer_get_time() / 1000) + kIntervalMs;
        PlayBatteryEmotion("low_battery", 4000);
    }

    void HandleBatteryEmotions()
    {
        if (charge_ == nullptr) {
            return;
        }

        Charge::BatteryInfo battery_info;
        if (!charge_->GetBatteryInfo(battery_info)) {
            return;
        }
        const int level = battery_info.level;
        const bool charging = battery_info.charging;

        if (charging != was_charging_) {
            Application::GetInstance().Schedule([]() {
                Board::GetInstance().GetDisplay()->UpdateStatusBar();
            });
        }
        if (charging && !was_charging_) {
            PlayBatteryEmotion("battery_connected", 4000);
            low_battery_alert_mask_ = 0;
            low_battery_plays_left_ = 0;
            next_low_battery_play_ms_ = 0;
        }
        was_charging_ = charging;

        if (charging) {
            return;
        }

        if (level > 12) {
            low_battery_alert_mask_ = 0;
            low_battery_plays_left_ = 0;
            next_low_battery_play_ms_ = 0;
            return;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (low_battery_plays_left_ > 0 && now_ms >= next_low_battery_play_ms_) {
            PlayBatteryEmotion("low_battery", 4000);
            low_battery_plays_left_--;
            if (low_battery_plays_left_ > 0) {
                next_low_battery_play_ms_ = now_ms + 5 * 60 * 1000;
            }
        }

        if (level <= 5 && (low_battery_alert_mask_ & 0x02) == 0) {
            low_battery_alert_mask_ |= 0x02;
            StartLowBatteryAlertSequence();
            return;
        }

        if (level <= 10 && (low_battery_alert_mask_ & 0x01) == 0) {
            low_battery_alert_mask_ |= 0x01;
            StartLowBatteryAlertSequence();
        }
    }

    static void battery_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        while (true) {
            if (self != nullptr && self->charge_ != nullptr && !self->touch_ui_busy_.load()) {
                if (self->charge_->Update()) {
                    self->HandleBatteryEmotions();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    static void imu_event_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr || !self->bmi270_ready_) {
            vTaskDelete(NULL);
            return;
        }

        struct bmi2_sens_data prev = {};
        struct bmi2_sens_data cur = {};
        bool has_prev = false;
        int64_t last_shake_ms = 0;
        constexpr int kShakeDeltaThreshold = 20000;
        constexpr int64_t kShakeCooldownMs = 2000;

        while (true) {
            if (self->touch_ui_busy_.load()) {
                has_prev = false;
                vTaskDelay(pdMS_TO_TICKS(80));
                continue;
            }
            const bool read_ok = Bmi270Motion::ReadAccelRaw(cur);
            if (read_ok) {
                if (has_prev) {
                    int dx = abs(static_cast<int>(cur.acc.x) - static_cast<int>(prev.acc.x));
                    int dy = abs(static_cast<int>(cur.acc.y) - static_cast<int>(prev.acc.y));
                    int dz = abs(static_cast<int>(cur.acc.z) - static_cast<int>(prev.acc.z));
                    int shake_score = dx + dy + dz;

                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (shake_score > kShakeDeltaThreshold && (now_ms - last_shake_ms) > kShakeCooldownMs) {
                        last_shake_ms = now_ms;
                        // "dizzy/nauseated" are not guaranteed in current assets, use supported fallback.
                        self->ShowTemporaryEmotion("confused", 1800);
                    }
                }
                prev = cur;
                has_prev = true;
            }
            vTaskDelay(pdMS_TO_TICKS(read_ok ? 80 : 250));
        }
    }

    void InitializeI2c()
    {
        i2c_config_t i2c_cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master = {
                .clk_speed = 400000,
            },
            .clk_flags = 0,
        };
        shared_i2c_bus_handle_ = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
        if (!shared_i2c_bus_handle_) {
            ESP_LOGE(TAG, "Failed to create shared I2C bus");
            ESP_ERROR_CHECK(ESP_FAIL);
        }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && !CONFIG_I2C_BUS_BACKWARD_CONFIG
        i2c_bus_ = i2c_bus_get_internal_bus_handle(shared_i2c_bus_handle_);
#else
#error "ESP-VoCat board requires i2c_bus_get_internal_bus_handle() support"
#endif
        if (!i2c_bus_) {
            ESP_LOGE(TAG, "Failed to get I2C master handle");
            ESP_ERROR_CHECK(ESP_FAIL);
        }

        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
        ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
    }
    uint8_t DetectPcbVersion()
        {
            gpio_config_t gpio_conf = {
                .pin_bit_mask = (1ULL << CORDEC_POWER_CTRL),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            ESP_ERROR_CHECK(gpio_config(&gpio_conf));
            ESP_ERROR_CHECK(gpio_set_level(CORDEC_POWER_CTRL, 0));
            vTaskDelay(pdMS_TO_TICKS(50));

            bool codec_alive = (i2c_master_probe(i2c_bus_, 0x18, 100) == ESP_OK);
            uint8_t pcb_version = 0;
            if (codec_alive) {
                ESP_LOGI(TAG, "PCB version V1.0");
                pcb_version = 0;
            } else {
                ESP_ERROR_CHECK(gpio_set_level(CORDEC_POWER_CTRL, 1));
                vTaskDelay(pdMS_TO_TICKS(50));
                codec_alive = (i2c_master_probe(i2c_bus_, 0x18, 100) == ESP_OK);
                if (codec_alive) {
                    ESP_LOGI(TAG, "PCB version V1.2");
                    pcb_version = 1;
                    AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_2;
                    AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_2;
                    QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_2;
                    TOUCH_PAD2 = TOUCH_PAD2_2;
                    UART1_TX = UART1_TX_2;
                    UART1_RX = UART1_RX_2;
                } else {
                    ESP_LOGE(TAG, "PCB version detection error");
                }
            }
            return pcb_version;
        }

    bool ProbeI2cDevice(uint8_t address, const char* device_name)
    {
        constexpr int kProbeAttempts = 3;
        esp_err_t ret = ESP_FAIL;
        for (int i = 0; i < kProbeAttempts; ++i) {
            ret = i2c_master_probe(i2c_bus_, address, 100);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "%s detected at I2C address 0x%02X", device_name, address);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGW(TAG, "%s not detected at I2C address 0x%02X: %s",
                 device_name, address, esp_err_to_name(ret));
        return false;
    }

    emote::EmoteDisplay* TouchDisplay()
    {
        return dynamic_cast<emote::EmoteDisplay*>(display_);
    }

    static int SliderValueFromX(int x)
    {
        return std::clamp((x - 50) * 100 / 260, 0, 100);
    }

    static const char* TouchPageName(TouchPage page)
    {
        switch (page) {
            case TouchPage::kHidden: return "hidden";
            case TouchPage::kPulling: return "pulling";
            case TouchPage::kMenu: return "menu";
            case TouchPage::kVolume: return "volume";
            case TouchPage::kBrightness: return "brightness";
            case TouchPage::kWallpaper: return "wallpaper";
            case TouchPage::kMusic: return "music";
            case TouchPage::kMusicPlayback: return "music_playback";
        }
        return "unknown";
    }

    static void TouchInterruptCallback(esp_lcd_touch_handle_t touch)
    {
        auto* self = touch ? static_cast<EspVocat*>(touch->config.user_data) : nullptr;
        if (self && self->touch_task_handle_) {
            BaseType_t higher_priority_task_woken = pdFALSE;
            vTaskNotifyGiveFromISR(self->touch_task_handle_, &higher_priority_task_woken);
            portYIELD_FROM_ISR(higher_priority_task_woken);
        }
    }

    void RenderMusicPage(bool force = false)
    {
        auto* emote_display = TouchDisplay();
        if (!emote_display || !music_player_) return;
        const auto snapshot = music_player_->GetUiSnapshot();
        if (snapshot.titles.empty()) {
            music_selected_index_ = -1;
        } else if (music_selected_index_ < 0 ||
                   music_selected_index_ >= static_cast<int>(snapshot.titles.size())) {
            music_selected_index_ = snapshot.current_index >= 0
                ? snapshot.current_index : 0;
            music_first_index_ = std::max(0, music_selected_index_ - 2);
        }
        const int max_first = std::max(0, static_cast<int>(snapshot.titles.size()) - 5);
        music_first_index_ = std::clamp(music_first_index_, 0, max_first);
        if (!force && snapshot.library_revision == music_library_revision_ &&
            snapshot.state_revision == music_state_revision_ &&
            music_first_index_ == rendered_music_first_index_ &&
            music_selected_index_ == rendered_music_selected_index_) {
            return;
        }
        const char* status = snapshot.library_ready ? "暂无歌曲"
                             : snapshot.library_error.empty() ? "正在读取音乐列表…"
                                                              : snapshot.library_error.c_str();
        const char* primary_control = snapshot.state == SdMusicPlayer::UiState::kPlaying
            ? "停止" : snapshot.state == SdMusicPlayer::UiState::kPaused ? "继续" : "播放";
        emote_display->ShowTouchMusic(snapshot.titles, music_first_index_,
                                      music_selected_index_, status, primary_control);
        music_library_revision_ = snapshot.library_revision;
        music_state_revision_ = snapshot.state_revision;
        rendered_music_first_index_ = music_first_index_;
        rendered_music_selected_index_ = music_selected_index_;
        music_ui_refresh_us_ = esp_timer_get_time();
    }

    void CloseTouchSettings()
    {
        ESP_LOGI(TAG, "Touch UI close from page=%s", TouchPageName(touch_page_));
        if (auto* emote_display = TouchDisplay()) {
            emote_display->HideTouchSettings();
        }
        touch_page_ = TouchPage::kHidden;
        touch_ui_busy_.store(false);
        slider_dragging_ = false;
    }

    void ShowTouchMenu(int reveal_height = DISPLAY_HEIGHT)
    {
        if (auto* emote_display = TouchDisplay()) {
            if (touch_page_ != TouchPage::kMenu && touch_page_ != TouchPage::kPulling) {
                touch_menu_status_[0] = std::to_string(GetAudioCodec()->output_volume()) + "%";
                touch_menu_status_[1] = std::to_string(backlight_ ? backlight_->brightness() : 75) + "%";
                touch_menu_status_[2] = emote_display->GetTouchWallpaperName(
                    emote_display->GetTouchWallpaperIndex());
                touch_menu_status_[3] = "未播放";
                if (music_player_) {
                    const auto snapshot = music_player_->GetUiSnapshot();
                    const int index = snapshot.current_index >= 0 ? snapshot.current_index : music_selected_index_;
                    if (index >= 0 && index < static_cast<int>(snapshot.titles.size()))
                        touch_menu_status_[3] = snapshot.titles[index];
                }
            }
            emote_display->ShowTouchMenu(reveal_height, touch_menu_status_[0].c_str(),
                                         touch_menu_status_[1].c_str(), touch_menu_status_[2].c_str(),
                                         touch_menu_status_[3].c_str());
            touch_page_ = reveal_height >= DISPLAY_HEIGHT ? TouchPage::kMenu : TouchPage::kPulling;
            touch_ui_busy_.store(true);
        }
    }

    void AnimateTouchMenu(int start_height, int end_height)
    {
        constexpr int kFrames = 6;
        for (int frame = 1; frame <= kFrames; ++frame) {
            const float t = static_cast<float>(frame) / kFrames;
            const float remaining = 1.0f - t;
            const float eased = 1.0f - remaining * remaining * remaining;
            ShowTouchMenu(static_cast<int>(start_height + (end_height - start_height) * eased));
            if (frame != kFrames) vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    void OpenTouchPage(int row)
    {
        auto* emote_display = TouchDisplay();
        if (!emote_display) return;
        ESP_LOGI(TAG, "Touch UI menu select row=%d", row);
        if (row == 0) {
            touch_page_ = TouchPage::kVolume;
            touch_value_ = GetAudioCodec()->output_volume();
            emote_display->ShowTouchSlider("音量调节", touch_value_);
        } else if (row == 1) {
            touch_page_ = TouchPage::kBrightness;
            touch_value_ = backlight_ ? backlight_->brightness() : 75;
            emote_display->ShowTouchSlider("亮度调节", touch_value_);
        } else if (row == 2) {
            touch_page_ = TouchPage::kWallpaper;
            const int count = emote_display->GetTouchWallpaperCount();
            wallpaper_candidate_ = count > 0
                ? std::clamp(emote_display->GetTouchWallpaperIndex(), 0, count - 1) : 0;
            emote_display->ShowTouchWallpaper(wallpaper_candidate_);
        } else if (row == 3) {
            touch_page_ = TouchPage::kMusic;
            const auto snapshot = music_player_->GetUiSnapshot();
            music_selected_index_ = snapshot.current_index;
            music_first_index_ = std::max(0, music_selected_index_ - 2);
            music_player_->RequestLibraryRefresh();
            RenderMusicPage(true);
        }
    }

    void PreviewWallpaper(int direction)
    {
        auto* emote_display = TouchDisplay();
        if (!emote_display) return;
        const int count = emote_display->GetTouchWallpaperCount();
        if (count <= 0) return;
        wallpaper_candidate_ = (wallpaper_candidate_ + direction + count) % count;
        const int candidate = wallpaper_candidate_;
        Application::GetInstance().Schedule([this, emote_display, candidate]() {
            if (touch_page_ == TouchPage::kWallpaper && candidate == wallpaper_candidate_) {
                const int64_t started_us = esp_timer_get_time();
                const bool shown = emote_display->ShowTouchWallpaper(candidate);
                ESP_LOGI(TAG, "Wallpaper preview index=%d shown=%d decode=%lldms",
                         candidate, shown, (esp_timer_get_time() - started_us) / 1000);
            }
        });
    }

    void HandleScreenTouch(bool pressed, int x, int y)
    {
        auto& app = Application::GetInstance();
        if (touch_page_ != TouchPage::kHidden && app.GetDeviceState() != kDeviceStateIdle) {
            suppress_screen_release_ = screen_touch_down_;
            CloseTouchSettings();
        }

        if (pressed && !screen_touch_down_) {
            screen_touch_down_ = true;
            touch_ui_busy_.store(true);
            slider_dragging_ = false;
            touch_start_x_ = touch_last_x_ = x;
            touch_start_y_ = touch_last_y_ = y;
            screen_touch_start_us_ = esp_timer_get_time();
            ESP_LOGI(TAG, "Touch down page=%s x=%d y=%d", TouchPageName(touch_page_), x, y);
            if ((touch_page_ == TouchPage::kVolume || touch_page_ == TouchPage::kBrightness) &&
                y >= 155 && y <= 235) {
                slider_dragging_ = true;
                touch_value_ = SliderValueFromX(x);
                if (auto* emote_display = TouchDisplay()) {
                    emote_display->UpdateTouchSliderValue(touch_value_);
                }
                if (touch_page_ == TouchPage::kBrightness && backlight_) {
                    backlight_->SetBrightness(touch_value_, false);
                }
            }
            return;
        }

        if (pressed && screen_touch_down_) {
            touch_last_x_ = x;
            touch_last_y_ = y;
            const int64_t now_us = esp_timer_get_time();
            const bool render_frame = now_us - touch_render_us_ >= 33 * 1000;
            if (touch_page_ == TouchPage::kHidden && touch_start_y_ <= 30 &&
                app.GetDeviceState() == kDeviceStateIdle && y > touch_start_y_) {
                if (render_frame) ShowTouchMenu(y - touch_start_y_);
            } else if (touch_page_ == TouchPage::kPulling) {
                if (render_frame) ShowTouchMenu(y - touch_start_y_);
            } else if (slider_dragging_) {
                touch_value_ = SliderValueFromX(x);
                if (render_frame) {
                    if (auto* emote_display = TouchDisplay()) {
                        emote_display->UpdateTouchSliderValue(touch_value_);
                    }
                }
                if (touch_page_ == TouchPage::kBrightness && backlight_) {
                    backlight_->SetBrightness(touch_value_, false);
                }
            }
            if (render_frame) touch_render_us_ = now_us;
            return;
        }

        if (pressed || !screen_touch_down_) return;
        screen_touch_down_ = false;
        if (touch_page_ == TouchPage::kHidden) touch_ui_busy_.store(false);
        if (suppress_screen_release_) {
            suppress_screen_release_ = false;
            return;
        }
        const int dx = touch_last_x_ - touch_start_x_;
        const int dy = touch_last_y_ - touch_start_y_;
        const int64_t duration_ms = (esp_timer_get_time() - screen_touch_start_us_) / 1000;
        const bool tap = std::abs(dx) < 12 && std::abs(dy) < 12;
        ESP_LOGI(TAG, "Touch up page=%s start=(%d,%d) end=(%d,%d) delta=(%d,%d) duration=%lldms tap=%d",
                 TouchPageName(touch_page_), touch_start_x_, touch_start_y_,
                 touch_last_x_, touch_last_y_, dx, dy, duration_ms, tap);

        if (touch_page_ == TouchPage::kHidden) {
            if (!tap || duration_ms > 800) {
                ESP_LOGI(TAG, "Touch gesture ignored on hidden page");
                return;
            }
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            } else {
                app.ToggleChatState();
            }
            return;
        }
        if (touch_page_ == TouchPage::kPulling) {
            const int current_height = std::clamp(dy, 0, DISPLAY_HEIGHT);
            if (dy >= 110) {
                AnimateTouchMenu(current_height, DISPLAY_HEIGHT);
            } else {
                AnimateTouchMenu(current_height, 0);
                CloseTouchSettings();
            }
            return;
        }
        if (touch_page_ == TouchPage::kMenu) {
            if (dy < -60 || (tap && touch_start_x_ >= 220 && touch_start_y_ <= 100)) {
                AnimateTouchMenu(DISPLAY_HEIGHT, 0);
                CloseTouchSettings();
                return;
            }
            if (tap && touch_start_y_ >= 82 && touch_start_y_ < 256) {
                const int column = touch_start_x_ >= 43 && touch_start_x_ < 176 ? 0
                    : touch_start_x_ >= 184 && touch_start_x_ < 317 ? 1 : -1;
                const int card_row = touch_start_y_ < 164 ? 0
                    : touch_start_y_ >= 174 ? 1 : -1;
                if (column >= 0 && card_row >= 0) {
                    OpenTouchPage(card_row * 2 + column);
                }
            }
            return;
        }
        const bool back_tap = tap && touch_start_x_ <= 150 && touch_start_y_ <= 108;
        const bool back_swipe = touch_start_x_ <= 75 && dx >= 60 && std::abs(dx) > std::abs(dy);
        if (touch_page_ == TouchPage::kMusicPlayback && (back_tap || back_swipe)) {
            ESP_LOGI(TAG, "Touch music playback return to list");
            touch_page_ = TouchPage::kMusic;
            RenderMusicPage(true);
            return;
        }
        if (back_tap || back_swipe) {
            ShowTouchMenu();
            return;
        }
        if (touch_page_ == TouchPage::kVolume || touch_page_ == TouchPage::kBrightness) {
            if (slider_dragging_) {
                const int value = touch_value_;
                const bool volume = touch_page_ == TouchPage::kVolume;
                ESP_LOGI(TAG, "Touch slider commit type=%s value=%d", volume ? "volume" : "brightness", value);
                Application::GetInstance().Schedule([this, value, volume]() {
                    if (volume) GetAudioCodec()->SetOutputVolume(value);
                    else if (backlight_) backlight_->SetBrightness(value, true);
                });
            }
            slider_dragging_ = false;
            return;
        }
        if (touch_page_ == TouchPage::kWallpaper) {
            if (std::abs(dx) >= 40 && std::abs(dx) > std::abs(dy)) {
                ESP_LOGI(TAG, "Touch wallpaper browse direction=%s", dx < 0 ? "next" : "previous");
                PreviewWallpaper(dx < 0 ? 1 : -1);
            } else if (tap && touch_start_y_ >= 80 && touch_start_y_ <= 300) {
                auto* emote_display = TouchDisplay();
                const int candidate = wallpaper_candidate_;
                ESP_LOGI(TAG, "Touch wallpaper apply index=%d", candidate);
                Application::GetInstance().Schedule([this, emote_display, candidate]() {
                    if (touch_page_ == TouchPage::kWallpaper && emote_display) {
                        emote_display->ApplyTouchWallpaper(candidate);
                    }
                });
            }
            return;
        }
        if (touch_page_ == TouchPage::kMusic) {
            if (std::abs(dy) >= 35 && std::abs(dy) > std::abs(dx)) {
                const auto snapshot = music_player_->GetUiSnapshot();
                const int max_first = std::max(0, static_cast<int>(snapshot.titles.size()) - 5);
                music_first_index_ = std::clamp(music_first_index_ + (dy < 0 ? 1 : -1), 0, max_first);
                ESP_LOGI(TAG, "Touch music scroll first=%d", music_first_index_);
                RenderMusicPage();
            } else if (tap && touch_start_y_ >= 88 && touch_start_y_ < 268) {
                const int index = music_first_index_ + (touch_start_y_ - 88) / 36;
                const auto snapshot = music_player_->GetUiSnapshot();
                if (index >= 0 && index < static_cast<int>(snapshot.titles.size())) {
                    ESP_LOGI(TAG, "Touch music select index=%d", index);
                    music_selected_index_ = index;
                    RenderMusicPage();
                }
            } else if (tap && touch_start_x_ >= 60 && touch_start_x_ <= 300 &&
                       touch_start_y_ >= 278 && touch_start_y_ <= 328) {
                const int control = std::clamp((touch_start_x_ - 66) / 76, 0, 2);
                ESP_LOGI(TAG, "Touch music control=%d", control);
                const auto snapshot = music_player_->GetUiSnapshot();
                if (snapshot.titles.empty()) {
                    return;
                }
                if (control == 0 || control == 2) {
                    const int direction = control == 0 ? -1 : 1;
                    music_selected_index_ = std::clamp(
                        music_selected_index_ + direction, 0,
                        static_cast<int>(snapshot.titles.size()) - 1);
                    if (music_selected_index_ < music_first_index_) {
                        music_first_index_ = music_selected_index_;
                    } else if (music_selected_index_ >= music_first_index_ + 5) {
                        music_first_index_ = music_selected_index_ - 4;
                    }
                    ESP_LOGI(TAG, "Touch music selection moved index=%d", music_selected_index_);
                    RenderMusicPage();
                    return;
                }
                if (music_selected_index_ < 0 ||
                    music_selected_index_ >= static_cast<int>(snapshot.titles.size())) {
                    return;
                }
                if (snapshot.state == SdMusicPlayer::UiState::kPlaying) {
                    ESP_LOGI(TAG, "Touch music stop playback");
                    Application::GetInstance().Schedule([this]() {
                        music_player_->Stop();
                    });
                    return;
                }
                const int selected = music_selected_index_;
                touch_page_ = TouchPage::kMusicPlayback;
                if (auto* emote_display = TouchDisplay()) {
                    emote_display->ShowTouchMusicPlayback();
                }
                ESP_LOGI(TAG, "Touch music play selected index=%d", selected);
                Application::GetInstance().Schedule([this, selected]() {
                    const auto current = music_player_->GetUiSnapshot();
                    if (current.current_index != selected ||
                        current.state == SdMusicPlayer::UiState::kStopped) {
                        music_player_->PlayIndex(selected);
                    } else if (current.state == SdMusicPlayer::UiState::kPaused) {
                        music_player_->TogglePauseResume();
                    }
                });
            }
        }
    }

    static void touch_event_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        while (self && self->tp_) {
            bool should_read = self->screen_touch_down_;
            if (should_read) {
                vTaskDelay(pdMS_TO_TICKS(16));
            } else {
                should_read = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20)) > 0;
            }
            if (should_read) {
                esp_lcd_touch_point_data_t point = {};
                uint8_t point_count = 0;
                if (esp_lcd_touch_read_data(self->tp_) == ESP_OK) {
                    esp_lcd_touch_get_data(self->tp_, &point, &point_count, 1);
                    self->HandleScreenTouch(point_count > 0, point.x, point.y);
                }
            }
            if (self->touch_page_ == TouchPage::kMusic &&
                esp_timer_get_time() - self->music_ui_refresh_us_ >= 250 * 1000) {
                self->RenderMusicPage();
            }
            if (!self->screen_touch_down_) vTaskDelay(pdMS_TO_TICKS(4));
        }
        vTaskDelete(nullptr);
    }

    void InitializeCharge()
    {
        if (!ProbeI2cDevice(0x55, "Charge IC")) {
            ESP_LOGW(TAG, "Battery measurement disabled");
            return;
        }
        charge_ = new Charge(i2c_bus_, 0x55);
        xTaskCreatePinnedToCore(battery_task, "batteryTask", 3 * 1024, this, 6, &charge_task_handle_, 0);
    }

    void InitializeCst816sTouchPad()
    {
        if (!ProbeI2cDevice(0x15, "Touch IC")) {
            ESP_LOGW(TAG, "Touch input disabled");
            return;
        }
        const esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 0,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 1,
            },
            .scl_speed_hz = 400 * 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &touch_io_));
        const esp_lcd_touch_config_t touch_config = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TP_PIN_NUM_RST,
            .int_gpio_num = TP_PIN_NUM_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
            .user_data = this,
        };
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(touch_io_, &touch_config, &tp_));
        xTaskCreatePinnedToCore(touch_event_task, "touch_task", 6 * 1024, this, 5,
                                &touch_task_handle_, 1);
        ESP_ERROR_CHECK(esp_lcd_touch_register_interrupt_callback(tp_, TouchInterruptCallback));
        ESP_LOGI(TAG, "CST816S touch settings input initialized");
    }

    void InitializeBmi270()
    {
        esp_err_t imu_ret = Bmi270Motion::Initialize(shared_i2c_bus_handle_);
        if (imu_ret == ESP_OK) {
            bmi270_ready_ = true;
            xTaskCreatePinnedToCore(imu_event_task, "imu_task", 4 * 1024, this, 4, &imu_task_handle_, 1);
        } else {
            ESP_LOGW(TAG, "BMI270 unavailable, shake emotion disabled");
        }
    }

    static uint32_t TouchChannelFromPadGpio(gpio_num_t gpio)
    {
        if (gpio == GPIO_NUM_NC) {
            return 0;
        }
        if (gpio >= GPIO_NUM_1 && gpio <= GPIO_NUM_14) {
            return static_cast<uint32_t>(gpio);
        }
        return 0;
    }

    static void touch_button_event_callback(touch_button_handle_t handle, uint32_t channel, touch_state_t state, void* cb_arg)
    {
        (void)handle;
        auto* self = static_cast<EspVocat*>(cb_arg);
        if (self == nullptr || self->display_ == nullptr) {
            return;
        }
        if (!self->two_pad_touch_mode_) {
            if (state == TOUCH_STATE_ACTIVE) {
                ESP_LOGI(TAG, "Touch button ACTIVE ch=%" PRIu32, channel);
                self->ShowTouchFeedback("happy");
            }
            return;
        }

        const uint8_t channel_mask = channel == self->touch_pad1_channel_ ? BIT0 :
                                     channel == self->touch_pad2_channel_ ? BIT1 : 0;
        if (channel_mask == 0) {
            return;
        }
        const int64_t now_us = esp_timer_get_time();
        if (state == TOUCH_STATE_ACTIVE) {
            if (self->touch_release_pending_) {
                constexpr int64_t kTransitionWindowUs = 450 * 1000;
                if (now_us - self->touch_release_time_us_ <= kTransitionWindowUs &&
                    channel != self->touch_start_channel_) {
                    self->touch_swipe_detected_ = true;
                    self->touch_release_pending_ = false;
                    ESP_LOGI(TAG, "Touch pads switched ch%" PRIu32 " -> ch%" PRIu32,
                             self->touch_start_channel_, channel);
                } else {
                    self->ShowTouchFeedback(self->touch_swipe_detected_ ? "shocked" : "happy");
                    self->touch_swipe_detected_ = false;
                    self->touch_release_pending_ = false;
                    self->touch_start_channel_ = channel;
                    self->touch_start_time_us_ = now_us;
                }
            } else if (self->touch_active_mask_ == 0) {
                self->touch_start_channel_ = channel;
                self->touch_start_time_us_ = now_us;
                self->touch_swipe_detected_ = false;
                ESP_LOGI(TAG, "Touch pads start ch=%" PRIu32, channel);
            } else if (channel != self->touch_start_channel_ && now_us - self->touch_start_time_us_ >= 80 * 1000) {
                self->touch_swipe_detected_ = true;
                ESP_LOGI(TAG, "Touch pads overlapped ch%" PRIu32 " -> ch%" PRIu32,
                         self->touch_start_channel_, channel);
            }
            self->touch_active_mask_ |= channel_mask;
        } else {
            self->touch_active_mask_ &= ~channel_mask;
            if (self->touch_active_mask_ == 0) {
                self->touch_release_pending_ = true;
                self->touch_release_time_us_ = now_us;
            }
        }
    }

    void RegisterWallpaperMcpTools()
    {
        auto& server = McpServer::GetInstance();
        server.AddTool(
            "self.wallpaper.set_location",
            "Save the city explicitly provided by the user for the Emote idle wallpaper. "
            "Call this when the user states where they are or asks to set the wallpaper city.",
            PropertyList({Property("city", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                auto* display = dynamic_cast<emote::EmoteDisplay*>(display_);
                if (!display) {
                    return std::string("Wallpaper desktop is only available in the Emote firmware.");
                }
                const std::string city = properties["city"].value<std::string>();
                if (city.empty()) {
                    return std::string("City is required.");
                }
                display->SetWallpaperLocation(city.c_str());
                return std::string("Wallpaper city saved: ") + city;
            });

          server.AddTool(
              "self.wallpaper.update_weather",
            "After the weather service has returned a result, update the cached Emote wallpaper weather. "
            "Do not call a weather service from the device; pass the service result to this tool.",
            PropertyList({Property("city", kPropertyTypeString),
                          Property("condition", kPropertyTypeString),
                          Property("temperature_c", kPropertyTypeInteger, -100, 100),
                          Property("high_c", kPropertyTypeInteger, -1000),
                          Property("low_c", kPropertyTypeInteger, -1000)}),
            [this](const PropertyList& properties) -> ReturnValue {
                auto* display = dynamic_cast<emote::EmoteDisplay*>(display_);
                if (!display) {
                    return std::string("Wallpaper desktop is only available in the Emote firmware.");
                }
                const std::string city = properties["city"].value<std::string>();
                const std::string condition = properties["condition"].value<std::string>();
                if (city.empty() || condition.empty()) {
                    return std::string("City and condition are required.");
                }
                display->SetWallpaperWeather(city.c_str(), condition.c_str(),
                                             properties["temperature_c"].value<int>(),
                                             properties["high_c"].value<int>(),
                                             properties["low_c"].value<int>());
                  return std::string("Wallpaper weather updated: ") + city + " " + condition;
              });

          server.AddTool(
              "self.wallpaper.refresh",
              "Rescan /wallpapers on the SD card for JPEG custom wallpapers. Call this when the user asks to refresh or reload wallpaper files.",
              PropertyList(),
              [this](const PropertyList&) -> ReturnValue {
                  auto* display = dynamic_cast<emote::EmoteDisplay*>(display_);
                  if (!display) {
                      return std::string("Wallpaper desktop is only available in the Emote firmware.");
                  }
                  return display->RefreshCustomWallpapers();
              });
      }

    void RegisterMediaTransferMcpTools()
    {
        auto& server = McpServer::GetInstance();
        server.AddTool(
            "self.media_transfer.start",
            "Open the Emote device's local Wi-Fi wallpaper management page for 10 minutes. Call when the user asks to upload or manage SD-card wallpapers. The device displays the QR code and IP address for a phone or computer browser.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue { return media_transfer_->Start(); });
        server.AddTool(
            "self.media_transfer.stop", "Close the local Wi-Fi media transfer page.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue { return media_transfer_->Stop(); });
        server.AddTool(
            "self.media_transfer.status", "Get the local Wi-Fi media transfer status.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue { return media_transfer_->Status(); });
    }

    void HandleTouchRelease()
    {
        constexpr int64_t kTransitionWindowUs = 450 * 1000;
        if (two_pad_touch_mode_ && touch_release_pending_ &&
            esp_timer_get_time() - touch_release_time_us_ > kTransitionWindowUs) {
            ShowTouchFeedback(touch_swipe_detected_ ? "shocked" : "happy");
            touch_swipe_detected_ = false;
            touch_release_pending_ = false;
            touch_start_channel_ = 0;
        }
    }

    static void touch_cap_poll_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        while (true) {
            if (self != nullptr && self->touch_button_handle_ != nullptr) {
                touch_button_sensor_handle_events(self->touch_button_handle_);
                self->HandleTouchRelease();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void InitializeCapacitiveTouchPads()
    {
        if (TOUCH_PAD1 == GPIO_NUM_NC) {
            ESP_LOGW(TAG, "Capacitive touch disabled: TOUCH_PAD1 NC");
            return;
        }

        const uint32_t ch1 = TouchChannelFromPadGpio(TOUCH_PAD1);
        if (ch1 == 0) {
            ESP_LOGW(TAG, "TOUCH_PAD1 GPIO %d is not a touch channel (expect GPIO1..GPIO14)", (int)TOUCH_PAD1);
            return;
        }

        if (TOUCH_PAD2 != GPIO_NUM_NC) {
            const uint32_t ch2 = TouchChannelFromPadGpio(TOUCH_PAD2);
            if (ch2 == 0) {
                ESP_LOGW(TAG, "TOUCH_PAD2 GPIO %d is not a touch channel", (int)TOUCH_PAD2);
                return;
            }

            static uint32_t btn_ch[2];
            static float btn_thr[2];
            btn_ch[0] = ch1;
            btn_ch[1] = ch2;
            btn_thr[0] = 0.012f;
            btn_thr[1] = 0.012f;

            touch_button_config_t btn_cfg = {
                .channel_num = 2,
                .channel_list = btn_ch,
                .channel_threshold = btn_thr,
                .channel_gold_value = nullptr,
                .debounce_times = 3,
                .skip_lowlevel_init = false,
            };
            esp_err_t err = touch_button_sensor_create(&btn_cfg, &touch_button_handle_, touch_button_event_callback, this);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "touch_button_sensor_create failed: %s", esp_err_to_name(err));
                touch_button_handle_ = nullptr;
                return;
            }
            two_pad_touch_mode_ = true;
            touch_pad1_channel_ = ch1;
            touch_pad2_channel_ = ch2;
            xTaskCreatePinnedToCore(touch_cap_poll_task, "touch_cap", 3072, this, 3, &touch_slider_task_handle_, 1);
            ESP_LOGI(TAG, "Touch pads (PCB v1.2+): PAD1 GPIO%d ch%u, PAD2 GPIO%d ch%u",
                     (int)TOUCH_PAD1, (unsigned)btn_ch[0], (int)TOUCH_PAD2, (unsigned)btn_ch[1]);
            return;
        }

        static uint32_t btn_ch[1];
        static float btn_thr[1];
        btn_ch[0] = ch1;
        btn_thr[0] = 0.004f;

        touch_button_config_t btn_cfg = {
            .channel_num = 1,
            .channel_list = btn_ch,
            .channel_threshold = btn_thr,
            .channel_gold_value = nullptr,
            .debounce_times = 2,
            .skip_lowlevel_init = false,
        };
        esp_err_t err = touch_button_sensor_create(&btn_cfg, &touch_button_handle_, touch_button_event_callback, this);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch_button_sensor_create failed: %s", esp_err_to_name(err));
            touch_button_handle_ = nullptr;
            return;
        }
        xTaskCreatePinnedToCore(touch_cap_poll_task, "touch_cap", 3072, this, 3, &touch_slider_task_handle_, 1);
        ESP_LOGI(TAG, "Touch button (PCB v1.0): TOUCH_PAD1 GPIO%d ch%u", (int)TOUCH_PAD1, (unsigned)btn_ch[0]);
    }

    void InitializeSpi()
    {
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                                  QSPI_PIN_NUM_LCD_DATA0,
                                                                                  QSPI_PIN_NUM_LCD_DATA1,
                                                                                  QSPI_PIN_NUM_LCD_DATA2,
                                                                                  QSPI_PIN_NUM_LCD_DATA3,
                                                                                  QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeSt77916Display(uint8_t pcb_version)
    {

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        const esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(QSPI_PIN_NUM_LCD_CS, NULL, NULL);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));
        st77916_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init_yysj,
            .init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
            .flags = {
                .reset_active_high = pcb_version,
            },
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
        backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        backlight_->RestoreBrightness();
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ESP_LOGI(TAG, "Boot button pressed, enter WiFi configuration mode");
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        gpio_config_t power_gpio_config = {
            .pin_bit_mask = (BIT64(POWER_CTRL)),
            .mode = GPIO_MODE_OUTPUT,

        };
        ESP_ERROR_CHECK(gpio_config(&power_gpio_config));

        gpio_set_level(POWER_CTRL, 0);
    }

#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    void InitializeCamera() {
        esp_video_init_usb_uvc_config_t usb_uvc_config = {
            .uvc = {
                .uvc_dev_num = 1,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
            .usb = {
                .init_usb_host_lib = true,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
        };

        esp_video_init_config_t video_config = {
            .usb_uvc = &usb_uvc_config,
        };

        camera_ = new EspVideo(video_config);
    }
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE

public:
    ~EspVocat() {
        if (touch_task_handle_ != nullptr) {
            vTaskDelete(touch_task_handle_);
            touch_task_handle_ = nullptr;
        }
        media_transfer_.reset();
        // Music owns a worker that may update the display, so stop it before deleting display objects.
        music_player_.reset();

        // Stop tasks
        if (charge_task_handle_ != nullptr) {
            vTaskDelete(charge_task_handle_);
        }
        if (imu_task_handle_ != nullptr) {
            vTaskDelete(imu_task_handle_);
        }
        if (touch_slider_task_handle_ != nullptr) {
            vTaskDelete(touch_slider_task_handle_);
            touch_slider_task_handle_ = nullptr;
        }
        if (touch_button_handle_ != nullptr) {
            touch_button_sensor_delete(touch_button_handle_);
            touch_button_handle_ = nullptr;
        }

        // Delete objects
        delete charge_;
        if (tp_ != nullptr) {
            esp_lcd_touch_del(tp_);
            tp_ = nullptr;
        }
        if (touch_io_ != nullptr) {
            esp_lcd_panel_io_del(touch_io_);
            touch_io_ = nullptr;
        }
        delete display_;
        // Note: backlight_ (PwmBacklight) and camera_ (EspVideo) are not deleted here
        // because their base classes (Backlight, Camera) don't have virtual destructors.
        // Since EspVocat is a singleton that lives for the device lifetime, this is acceptable.

        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
            esp_timer_delete(emotion_reset_timer_);
            emotion_reset_timer_ = nullptr;
        }

        // Disable temperature sensor
        if (temp_sensor != NULL) {
            temperature_sensor_disable(temp_sensor);
            temperature_sensor_uninstall(temp_sensor);
            temp_sensor = NULL;
        }
    }

    EspVocat() : boot_button_(BOOT_BUTTON_GPIO)
    {
        const esp_timer_create_args_t emotion_timer_args = {
            .callback = &EspVocat::emotion_reset_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "emotion_rst",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&emotion_timer_args, &emotion_reset_timer_));

        InitializeI2c();
        uint8_t pcb_version = DetectPcbVersion();
        InitializeCharge();
        InitializeBmi270();

        InitializeSpi();
        InitializeSt77916Display(pcb_version);
        InitializeButtons();
        InitializeCapacitiveTouchPads();
        SdCardManager::GetInstance().Initialize({
            .mount_point = "/sdcard",
            .clk = SD_CARD_CLK_GPIO,
            .cmd = SD_CARD_CMD_GPIO,
            .d0 = SD_CARD_D0_GPIO,
            .max_files = 6,
        });
        music_player_ = std::make_unique<SdMusicPlayer>();
        music_player_->RegisterMcpTools();
        InitializeCst816sTouchPad();
        RegisterWallpaperMcpTools();
        if (auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display_)) {
            emote_display->RefreshCustomWallpapers();
            media_transfer_ = std::make_unique<MediaTransferServer>(
                [emote_display]() { emote_display->RequestCustomWallpaperRefresh(); },
                [this]() { music_player_->RequestLibraryRefresh(); },
                [this]() { return music_player_->IsActive(); },
                [emote_display](const std::string& url) { emote_display->ShowMediaTransferQr(url.c_str()); },
                [emote_display](const std::string&) { emote_display->HideMediaTransferQr(); });
            RegisterMediaTransferMcpTools();
        }
#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
        InitializeCamera();
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }

    esp_lcd_touch_handle_t GetTouchpad()
    {
        return tp_;
    }

    virtual Backlight* GetBacklight() override
    {
        return backlight_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        Charge::BatteryInfo battery_info;
        if (charge_ == nullptr) {
            return false;
        }
        charge_->GetBatteryInfo(battery_info);
        level = battery_info.level;
        charging = battery_info.charging;
        discharging = battery_info.discharging;
        return true;
    }

    virtual bool GetBatteryDetail(int& level, bool& charging, bool& discharging,
                                  int& voltage_mv, int& current_ma) override {
        Charge::BatteryInfo battery_info;
        if (charge_ == nullptr) {
            return false;
        }
        charge_->GetBatteryInfo(battery_info);
        level = battery_info.level;
        charging = battery_info.charging;
        discharging = battery_info.discharging;
        voltage_mv = battery_info.voltage_mv;
        current_ma = battery_info.current_ma;
        return true;
    }
};

DECLARE_BOARD(EspVocat);
