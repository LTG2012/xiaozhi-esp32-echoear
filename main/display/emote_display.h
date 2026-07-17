#pragma once

#include "display.h"
#include <memory>
#include <string>
#include <vector>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"
#include "widget/gfx_img.h"

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
    virtual void ClearMusicLyrics() override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    static constexpr size_t kMusicLyricRowCount = 7;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    emote_handle_t emote_handle_ = nullptr;
    gfx_font_t music_font_ = nullptr;
    bool music_font_owned_ = false;
    gfx_obj_t* music_background_ = nullptr;
    gfx_obj_t* music_title_ = nullptr;
    gfx_obj_t* music_lyric_rows_[kMusicLyricRowCount] = {};
    gfx_obj_t* music_progress_ = nullptr;
    gfx_image_dsc_t music_progress_image_ = {};
    std::vector<uint8_t> music_progress_image_data_;
    std::vector<uint32_t> music_progress_pixels_;
    std::vector<uint16_t> music_progress_thresholds_;
    std::vector<uint8_t> music_progress_pixel_alphas_;
    uint16_t music_progress_track_color_ = 0;
    uint16_t music_progress_active_color_ = 0;
    bool music_progress_visible_ = false;

    bool EnsureMusicUi();
    bool InitializeMusicProgressImage();

};

} // namespace emote
