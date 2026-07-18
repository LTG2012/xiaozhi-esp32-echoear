#pragma once

#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <sdmmc_cmd.h>

struct SdCardMountConfig {
    const char* mount_point = "/sdcard";
    gpio_num_t clk = GPIO_NUM_NC;
    gpio_num_t cmd = GPIO_NUM_NC;
    gpio_num_t d0 = GPIO_NUM_NC;
    int max_files = 4;
};

class SdCardManager {
public:
    static SdCardManager& GetInstance();

    void Initialize(const SdCardMountConfig& config);
    std::string EnsureMounted();
    void MarkUnavailable();

    bool IsMounted() const;
    const char* MountPoint() const;
    std::mutex& FilesystemMutex() { return filesystem_mutex_; }

private:
    SdCardManager() = default;

    mutable std::mutex state_mutex_;
    std::mutex filesystem_mutex_;
    SdCardMountConfig config_;
    sdmmc_card_t* card_ = nullptr;
    bool initialized_ = false;
    bool mounted_ = false;
};
