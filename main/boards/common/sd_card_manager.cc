#include "sd_card_manager.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>

namespace {
constexpr char kTag[] = "SdCardManager";
}

SdCardManager& SdCardManager::GetInstance()
{
    static SdCardManager instance;
    return instance;
}

void SdCardManager::Initialize(const SdCardMountConfig& config)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    config_ = config;
    initialized_ = true;
}

std::string SdCardManager::EnsureMounted()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (mounted_) {
        return {};
    }
    if (!initialized_) {
        return "SD card is not configured for this board.";
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = config_.max_files,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = config_.clk;
    slot.cmd = config_.cmd;
    slot.d0 = config_.d0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(kTag, "Mounting 1-bit SDMMC: CLK=%d CMD=%d D0=%d",
             config_.clk, config_.cmd, config_.d0);
    const esp_err_t result = esp_vfs_fat_sdmmc_mount(config_.mount_point, &host, &slot,
                                                     &mount_config, &card_);
    if (result != ESP_OK) {
        card_ = nullptr;
        mounted_ = false;
        ESP_LOGW(kTag, "SD mount failed without formatting: %s", esp_err_to_name(result));
        return "Unable to mount SD card. Insert a FAT32 card; the firmware will not format it.";
    }
    mounted_ = true;
    sdmmc_card_print_info(stdout, card_);
    return {};
}

void SdCardManager::MarkUnavailable()
{
    std::lock_guard<std::mutex> filesystem_lock(filesystem_mutex_);
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount(config_.mount_point, card_);
    }
    card_ = nullptr;
    mounted_ = false;
}

bool SdCardManager::IsMounted() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return mounted_;
}

const char* SdCardManager::MountPoint() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return config_.mount_point;
}
