#pragma once

#include "display.h"
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"
#include "widget/gfx_img.h"
#include "widget/gfx_qrcode.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetMusicLyrics(const char* title, const char* lyrics) override;
    virtual void SetMusicProgress(int progress_permille) override;
    virtual void SetMusicPlaybackInfo(int elapsed_seconds, int total_seconds,
                                      int level_0, int level_1, int level_2) override;
    virtual void ClearMusicLyrics() override;
    virtual void SetWallpaperLocation(const char* city) override;
    virtual void SetWallpaperWeather(const char* city, const char* condition,
                                     int temperature_c, int high_c, int low_c) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);
    std::string RefreshCustomWallpapers();
    void RequestCustomWallpaperRefresh();
    bool ShowMediaTransferQr(const char* url);
    void HideMediaTransferQr();

    void ShowTouchMenu(int reveal_height = 360);
    void ShowTouchSlider(const char* title, int value);
    bool ShowTouchWallpaper(int index);
    void ShowTouchMusic(const std::vector<std::string>& titles, int first_index,
                        int selected_index, const char* status);
    void ShowTouchMusicPlayback();
    void HideTouchSettings();
    int GetTouchWallpaperCount();
    int GetTouchWallpaperIndex() const { return wallpaper_index_; }
    std::string GetTouchWallpaperName(int index);
    bool ApplyTouchWallpaper(int index);

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    static constexpr size_t kMusicLyricRowCount = 7;
    static constexpr int kWallpaperUnsetTemperature = -1000;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    emote_handle_t emote_handle_ = nullptr;
    gfx_font_t music_font_ = nullptr;
    bool music_font_owned_ = false;
    gfx_obj_t* music_background_ = nullptr;
    gfx_obj_t* music_title_ = nullptr;
    gfx_obj_t* music_lyric_rows_[kMusicLyricRowCount] = {};
    gfx_obj_t* music_time_ = nullptr;
    gfx_image_dsc_t music_time_image_ = {};
    std::vector<uint8_t> music_time_image_data_;
    gfx_obj_t* music_rhythm_bars_[6] = {};
    gfx_obj_t* music_progress_ = nullptr;
    gfx_image_dsc_t music_progress_image_ = {};
    std::vector<uint8_t> music_progress_image_data_;
    std::vector<uint32_t> music_progress_pixels_;
    std::vector<uint16_t> music_progress_thresholds_;
    std::vector<uint8_t> music_progress_pixel_alphas_;
    uint16_t music_progress_track_color_ = 0;
    uint16_t music_progress_active_color_ = 0;
    bool music_progress_visible_ = false;
    bool music_playback_info_visible_ = false;
    int music_elapsed_seconds_ = -1;
    int music_total_seconds_ = -1;
    int music_rhythm_heights_[3] = {};

    gfx_obj_t* wallpaper_image_ = nullptr;
    gfx_obj_t* wallpaper_fade_ = nullptr;
    gfx_obj_t* wallpaper_date_ = nullptr;
    gfx_obj_t* wallpaper_time_ = nullptr;
    gfx_obj_t* wallpaper_weather_ = nullptr;
    gfx_obj_t* media_transfer_qr_ = nullptr;
    gfx_obj_t* media_transfer_info_ = nullptr;
    bool media_transfer_qr_visible_ = false;
    gfx_font_t wallpaper_font_ = nullptr;
    bool wallpaper_font_owned_ = false;
    gfx_image_dsc_t wallpaper_image_dsc_ = {};
    std::mutex wallpaper_data_mutex_;
    std::vector<std::string> custom_wallpaper_paths_;
    std::vector<uint8_t> custom_wallpaper_pixels_;
    esp_timer_handle_t wallpaper_timer_ = nullptr;
    bool wallpaper_idle_ = false;
    bool wallpaper_music_active_ = false;
    bool wallpaper_visible_ = false;
    std::atomic<bool> wallpaper_refresh_requested_ = false;
    int wallpaper_index_ = 0;
    int wallpaper_last_minute_ = -1;
    int64_t wallpaper_idle_since_us_ = 0;
    int64_t wallpaper_shown_since_us_ = 0;
    int64_t wallpaper_fade_until_us_ = 0;
    int64_t wallpaper_suppressed_until_us_ = 0;
    std::string wallpaper_city_;
    std::string wallpaper_condition_;
    int wallpaper_temperature_c_ = kWallpaperUnsetTemperature;
    int wallpaper_high_c_ = kWallpaperUnsetTemperature;
    int wallpaper_low_c_ = kWallpaperUnsetTemperature;

    static constexpr size_t kTouchRowCount = 5;
    static constexpr size_t kTouchControlCount = 3;
    gfx_obj_t* touch_background_ = nullptr;
    gfx_obj_t* touch_wallpaper_image_ = nullptr;
    gfx_obj_t* touch_title_ = nullptr;
    gfx_obj_t* touch_back_ = nullptr;
    gfx_obj_t* touch_close_ = nullptr;
    std::array<gfx_obj_t*, kTouchRowCount> touch_rows_ = {};
    gfx_obj_t* touch_footer_ = nullptr;
    gfx_obj_t* touch_slider_track_ = nullptr;
    gfx_obj_t* touch_slider_fill_ = nullptr;
    gfx_obj_t* touch_slider_knob_ = nullptr;
    gfx_obj_t* touch_music_return_ = nullptr;
    std::array<gfx_obj_t*, kTouchControlCount> touch_controls_ = {};
    bool touch_settings_visible_ = false;
    bool touch_music_overlay_suppressed_ = false;
    bool touch_wallpaper_preview_ = false;
    bool touch_wallpaper_restore_visible_ = false;

    bool EnsureMusicUi();
    gfx_font_t EnsureCommonTextFont();
    bool InitializeMusicTimeImage();
    void RenderMusicTimeImage(const std::string& text);
    bool InitializeMusicProgressImage();
    void SetMusicOverlayVisible(bool visible);

    static void WallpaperTimerCallback(void* arg);
    bool EnsureWallpaperUi();
    bool LoadWallpaperAsset(int index);
    bool LoadCustomWallpaper(int index);
    bool LoadWallpaper(int index);
    size_t WallpaperCount();
    bool ScanCustomWallpapers(std::string& result);
    void SetWallpaperNativeUiVisible(bool visible);
    void ShowWallpaper();
    void HideWallpaper();
    void TickWallpaper();
    void UpdateWallpaperText(bool force = false);
    void CacheWeatherFromAssistantMessage(const char* content);
    void LoadWallpaperSettings();
    void SaveWallpaperSettings();
    bool EnsureTouchSettingsUi();
    void HideTouchObjects();
    void RestoreTouchWallpaperPreview();

};

} // namespace emote
