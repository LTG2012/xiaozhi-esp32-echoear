#include "emote_display.h"

// Standard C++ headers
#include <cstdio>
#include <cstring>
#include <memory>
#include <array>
#include <string>
#include <vector>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <cinttypes>
#include <cmath>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets/lang_config.h"
#include "assets.h"
#include "board.h"
#include "gfx.h"
#include "expression_emote.h"

extern "C" {
LV_FONT_DECLARE(font_puhui_basic_20_4);
}

namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";
static constexpr int kMusicStatusBarHeight = 70;
static constexpr size_t kMusicLyricRowCount = 7;
static constexpr int kMusicLyricRowHeight = 28;
static constexpr std::array<int, kMusicLyricRowCount> kMusicLyricRowY = {
    110, 140, 170, 200, 230, 260, 290,
};
static constexpr std::array<int, kMusicLyricRowCount> kMusicLyricRowWidth = {
    324, 340, 344, 340, 324, 292, 244,
};

struct MusicLyricSourceLine {
    std::string text;
    bool current = false;
};

struct MusicLyricVisualLine {
    std::string text;
    bool current = false;
    int estimated_width = 0;
};

static uint32_t DecodeUtf8Codepoint(const std::string& text, size_t& position, size_t& byte_count)
{
    const auto first = static_cast<uint8_t>(text[position]);
    byte_count = 1;
    if (first < 0x80) {
        ++position;
        return first;
    }

    if ((first & 0xE0) == 0xC0 && position + 1 < text.size()) {
        byte_count = 2;
        const uint32_t value = ((first & 0x1F) << 6) |
                               (static_cast<uint8_t>(text[position + 1]) & 0x3F);
        position += 2;
        return value;
    }
    if ((first & 0xF0) == 0xE0 && position + 2 < text.size()) {
        byte_count = 3;
        const uint32_t value = ((first & 0x0F) << 12) |
                               ((static_cast<uint8_t>(text[position + 1]) & 0x3F) << 6) |
                               (static_cast<uint8_t>(text[position + 2]) & 0x3F);
        position += 3;
        return value;
    }
    if ((first & 0xF8) == 0xF0 && position + 3 < text.size()) {
        byte_count = 4;
        const uint32_t value = ((first & 0x07) << 18) |
                               ((static_cast<uint8_t>(text[position + 1]) & 0x3F) << 12) |
                               ((static_cast<uint8_t>(text[position + 2]) & 0x3F) << 6) |
                               (static_cast<uint8_t>(text[position + 3]) & 0x3F);
        position += 4;
        return value;
    }

    ++position;
    return first;
}

static int EstimateMusicGlyphWidth(uint32_t codepoint)
{
    if (codepoint == ' ') {
        return 7;
    }
    if (codepoint < 0x80) {
        if (std::strchr(".,:;!'`|ijlI()[]{}", static_cast<int>(codepoint)) != nullptr) {
            return 7;
        }
        return 11;
    }
    return 20;
}

static std::vector<MusicLyricSourceLine> ParseMusicLyricSource(const char* lyrics)
{
    std::vector<MusicLyricSourceLine> result;
    std::string source = lyrics ? lyrics : "";
    size_t start = 0;
    while (start <= source.size()) {
        const size_t end = source.find('\n', start);
        std::string line = source.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const bool current = line.rfind("> ", 0) == 0;
        if (current) {
            line.erase(0, 2);
        }
        if (!line.empty()) {
            result.push_back({std::move(line), current});
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

static std::vector<MusicLyricVisualLine> LayoutMusicLyrics(
    const std::vector<MusicLyricSourceLine>& source_lines,
    const std::array<int, kMusicLyricRowCount>& row_widths,
    size_t start_source,
    int* current_first_row,
    bool* truncated)
{
    std::vector<MusicLyricVisualLine> rows;
    *current_first_row = -1;
    *truncated = false;

    size_t source_index = start_source;
    for (; source_index < source_lines.size() && rows.size() < kMusicLyricRowCount;
         ++source_index) {
        const auto& source = source_lines[source_index];
        const size_t first_row_for_source = rows.size();
        size_t position = 0;
        bool emitted = false;

        while (position < source.text.size() && rows.size() < kMusicLyricRowCount) {
            const size_t row_index = rows.size();
            const int width_limit = row_widths[row_index];
            std::string row_text;
            int row_width = 0;

            while (position < source.text.size()) {
                const size_t character_start = position;
                size_t byte_count = 0;
                const uint32_t codepoint = DecodeUtf8Codepoint(source.text, position, byte_count);
                const int glyph_width = EstimateMusicGlyphWidth(codepoint);
                if (!row_text.empty() && row_width + glyph_width > width_limit) {
                    position = character_start;
                    break;
                }
                if (row_text.empty() && codepoint == ' ') {
                    continue;
                }
                row_text.append(source.text, character_start, byte_count);
                row_width += glyph_width;
            }

            while (!row_text.empty() && row_text.back() == ' ') {
                row_text.pop_back();
                row_width -= 7;
            }
            if (source.current && *current_first_row < 0) {
                *current_first_row = static_cast<int>(rows.size());
            }
            rows.push_back({std::move(row_text), source.current, row_width});
            emitted = true;
        }

        if (!emitted && rows.size() < kMusicLyricRowCount) {
            if (source.current && *current_first_row < 0) {
                *current_first_row = static_cast<int>(rows.size());
            }
            rows.push_back({"", source.current, 0});
        }
        if (position < source.text.size()) {
            *truncated = true;
            if (!source.current) {
                // Never leave half of a future/previous lyric at the bottom of
                // the circular viewport. The active lyric keeps priority and
                // may use every row in the pathological case of a very long line.
                rows.resize(first_row_for_source);
            }
            break;
        }
    }
    if (source_index < source_lines.size()) {
        *truncated = true;
    }
    return rows;
}

static std::vector<MusicLyricVisualLine> BuildCircularMusicLyricRows(
    const char* lyrics,
    const std::array<int, kMusicLyricRowCount>& row_widths,
    bool* truncated)
{
    const auto source_lines = ParseMusicLyricSource(lyrics);
    if (source_lines.empty()) {
        *truncated = false;
        return {};
    }

    size_t current_source = source_lines.size();
    for (size_t i = 0; i < source_lines.size(); ++i) {
        if (source_lines[i].current) {
            current_source = i;
            break;
        }
    }

    size_t start_source = current_source < source_lines.size() && current_source > 0
                              ? current_source - 1
                              : 0;
    std::vector<MusicLyricVisualLine> rows;
    int current_first_row = -1;
    do {
        rows = LayoutMusicLyrics(source_lines, row_widths, start_source,
                                 &current_first_row, truncated);
        if (current_source >= source_lines.size() ||
            (current_first_row >= 0 && current_first_row <= 2)) {
            break;
        }
        ++start_source;
    } while (start_source <= current_source);

    if (current_source >= source_lines.size() && rows.size() == 1) {
        std::vector<MusicLyricVisualLine> centered(2);
        centered.push_back(std::move(rows.front()));
        return centered;
    }
    return rows;
}

static gfx_font_t LoadMusicFontFromAssets()
{
    void* index_data = nullptr;
    size_t index_size = 0;
    auto& assets = Assets::GetInstance();
    if (!assets.GetAssetData("index.json", index_data, index_size)) {
        return nullptr;
    }

    cJSON* root = cJSON_ParseWithLength(static_cast<const char*>(index_data), index_size);
    if (!root) {
        return nullptr;
    }
    cJSON* font_name_json = cJSON_GetObjectItem(root, "text_font");
    if (!cJSON_IsString(font_name_json)) {
        cJSON_Delete(root);
        return nullptr;
    }
    const std::string font_name = font_name_json->valuestring;
    cJSON_Delete(root);

    void* font_data = nullptr;
    size_t font_size = 0;
    if (!assets.GetAssetData(font_name, font_data, font_size) || !font_data || font_size == 0) {
        return nullptr;
    }
    return (gfx_font_t)gfx_font_lv_load_from_binary(static_cast<uint8_t*>(font_data));
}

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* const edata, void* user_ctx)
{
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel != nullptr) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    }
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width, const int height)
{
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = width,
            .v_res = height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = false,
        },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height)
{
    width_ = width;
    height_ = height;
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}

EmoteDisplay::~EmoteDisplay()
{
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
    if (music_font_owned_ && music_font_) {
        gfx_font_lv_delete(static_cast<lv_font_t*>(music_font_));
        music_font_ = nullptr;
        music_font_owned_ = false;
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (emote_handle_ && emotion && strlen(emotion) > 0) {
        emote_set_anim_emoji(emote_handle_, emotion);
    }
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (emote_handle_ && content && strlen(content) > 0) {
        if ((std::strcmp(role, "system") == 0) && std::strstr(content, "xiaozhi.me")) {
            size_t len = strlen(content);
            char* new_content = new char[len + 1];
            strcpy(new_content, content);
            std::replace(new_content, new_content + len, static_cast<char>(0x0A), static_cast<char>(0x20));
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, new_content);
            delete[] new_content;
        } else {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, content);
        }
    }
}

void EmoteDisplay::SetStatus(const char* const status)
{
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (emote_handle_ && status && strlen(status) > 0) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, NULL);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
        } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, NULL);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SET, NULL);
        }
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (emote_handle_ && notification && strlen(notification) > 0) {
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SYS, notification);
    }
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
        return;
    }

    char battery_status[16];
    snprintf(battery_status, sizeof(battery_status), "%d,%d", charging ? 1 : 0, battery_level);
    const esp_err_t ret = emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_BAT, battery_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update battery status: %s", esp_err_to_name(ret));
    }
}

void EmoteDisplay::ClearChatMessages()
{
    if (!emote_handle_) {
        return;
    }

    // Clearing a subtitle must not change the status icon. In particular,
    // EVT_SPEAK hides the battery indicator and replaces it with a speaker.
    gfx_obj_t* toast_label = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_TOAST_LABEL);
    if (toast_label != nullptr) {
        emote_lock(emote_handle_);
        gfx_label_set_text(toast_label, "");
        emote_unlock(emote_handle_);
    }
}

bool EmoteDisplay::EnsureMusicUi()
{
    if (!emote_handle_) {
        return false;
    }
    bool lyric_rows_ready = true;
    for (auto* row : music_lyric_rows_) {
        lyric_rows_ready = lyric_rows_ready && row != nullptr;
    }
    if (music_background_ && music_title_ && lyric_rows_ready && music_progress_) {
        return true;
    }

    music_background_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "music_background");
    music_title_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "music_title");
    for (size_t i = 0; i < kMusicLyricRowCount; ++i) {
        const std::string name = "music_lyric_" + std::to_string(i);
        music_lyric_rows_[i] = emote_create_obj_by_type(
            emote_handle_, EMOTE_OBJ_TYPE_LABEL, name.c_str());
    }
    music_progress_ = emote_create_obj_by_type(
        emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "music_progress");
    lyric_rows_ready = true;
    for (auto* row : music_lyric_rows_) {
        lyric_rows_ready = lyric_rows_ready && row != nullptr;
    }
    if (!music_background_ || !music_title_ || !lyric_rows_ready || !music_progress_ ||
        !InitializeMusicProgressImage()) {
        ESP_LOGE(TAG, "Failed to create music lyric UI");
        return false;
    }

    music_font_ = LoadMusicFontFromAssets();
    if (!music_font_) {
        music_font_ = (gfx_font_t)&font_puhui_basic_20_4;
        ESP_LOGW(TAG, "Emote common text font unavailable; using basic fallback");
    } else {
        music_font_owned_ = true;
    }

    emote_lock(emote_handle_);
    const int music_height = std::max(1, height_ - kMusicStatusBarHeight);

    gfx_obj_set_size(music_background_, width_, music_height);
    gfx_obj_align(music_background_, GFX_ALIGN_BOTTOM_MID, 0, 0);
    gfx_label_set_text(music_background_, "");
    gfx_label_set_bg_color(music_background_, GFX_COLOR_HEX(0x080A12));
    gfx_label_set_bg_enable(music_background_, true);

    gfx_obj_set_size(music_title_, std::max(1, std::min(width_ - 16, width_ * 300 / 360)), 32);
    gfx_obj_align(music_title_, GFX_ALIGN_TOP_MID, 0, kMusicStatusBarHeight + 4);
    gfx_label_set_font(music_title_, music_font_);
    gfx_label_set_color(music_title_, GFX_COLOR_HEX(0x62D9FF));
    gfx_label_set_text_align(music_title_, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(music_title_, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_loop(music_title_, true);
    gfx_label_set_scroll_speed(music_title_, 15);

    for (size_t i = 0; i < kMusicLyricRowCount; ++i) {
        const int row_width = std::max(1, std::min(width_ - 16, width_ * kMusicLyricRowWidth[i] / 360));
        const int row_y = height_ * kMusicLyricRowY[i] / 360;
        gfx_obj_set_size(music_lyric_rows_[i], row_width, kMusicLyricRowHeight);
        gfx_obj_align(music_lyric_rows_[i], GFX_ALIGN_TOP_MID, 0, row_y);
        gfx_label_set_font(music_lyric_rows_[i], music_font_);
        gfx_label_set_color(music_lyric_rows_[i], GFX_COLOR_HEX(0xFFFFFF));
        gfx_label_set_text_align(music_lyric_rows_[i], GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(music_lyric_rows_[i], GFX_LABEL_LONG_CLIP);
        gfx_label_set_text(music_lyric_rows_[i], "");
    }

    gfx_obj_set_visible(music_background_, false);
    gfx_obj_set_visible(music_title_, false);
    for (auto* row : music_lyric_rows_) {
        gfx_obj_set_visible(row, false);
    }
    gfx_img_set_src(music_progress_, &music_progress_image_);
    gfx_obj_set_pos(music_progress_, 0, height_ / 2 - 5);
    gfx_obj_set_visible(music_progress_, false);
    music_progress_visible_ = false;
    emote_unlock(emote_handle_);
    return true;
}

bool EmoteDisplay::InitializeMusicProgressImage()
{
    if (!music_progress_image_data_.empty()) {
        return true;
    }

    const int image_width = width_;
    const int image_y = height_ / 2 - 5;
    const int image_height = height_ - image_y;
    if (image_width <= 0 || image_height <= 0) {
        return false;
    }

    const size_t pixel_count = static_cast<size_t>(image_width) * image_height;
    music_progress_image_data_.assign(pixel_count * 3, 0);
    music_progress_pixels_.clear();
    music_progress_thresholds_.clear();
    music_progress_pixel_alphas_.clear();
    music_progress_track_color_ = gfx_color_hex(0xD7DCE2).full;
    music_progress_active_color_ = gfx_color_hex(0x62D9FF).full;

    auto* colors = reinterpret_cast<uint16_t*>(music_progress_image_data_.data());
    auto* alpha = music_progress_image_data_.data() + pixel_count * sizeof(uint16_t);
    const double center_x = width_ / 2.0;
    const double center_y = height_ / 2.0;
    const double radius = std::min(width_, height_) / 2.0 - 8.0;
    constexpr double half_width = 3.0;
    constexpr double pi = 3.14159265358979323846;

    for (int image_row = 0; image_row < image_height; ++image_row) {
        const double y = image_y + image_row + 0.5;
        const double dy = y - center_y;
        for (int x = 0; x < image_width; ++x) {
            const double dx = x + 0.5 - center_x;
            double distance_to_line = 1000.0;
            double fraction = 0.0;

            if (dy >= 0.0) {
                const double distance = std::sqrt(dx * dx + dy * dy);
                distance_to_line = std::abs(distance - radius);
                const double angle = std::atan2(dy, dx);
                fraction = (pi - angle) / pi;
            } else {
                const double left_dx = dx + radius;
                const double right_dx = dx - radius;
                const double left_distance = std::sqrt(left_dx * left_dx + dy * dy);
                const double right_distance = std::sqrt(right_dx * right_dx + dy * dy);
                if (left_distance <= right_distance) {
                    distance_to_line = left_distance;
                    fraction = 0.0;
                } else {
                    distance_to_line = right_distance;
                    fraction = 1.0;
                }
            }

            const double edge_alpha = half_width + 1.0 - distance_to_line;
            if (edge_alpha <= 0.0) {
                continue;
            }
            const size_t pixel = static_cast<size_t>(image_row) * image_width + x;
            colors[pixel] = music_progress_track_color_;
            const uint8_t pixel_alpha = static_cast<uint8_t>(
                std::min(255.0, std::max(0.0, edge_alpha * 255.0)));
            alpha[pixel] = static_cast<uint8_t>(
                (static_cast<uint16_t>(pixel_alpha) * 72U + 127U) / 255U);
            music_progress_pixels_.push_back(static_cast<uint32_t>(pixel));
            music_progress_thresholds_.push_back(static_cast<uint16_t>(
                std::lround(std::clamp(fraction, 0.0, 1.0) * 1000.0)));
            music_progress_pixel_alphas_.push_back(pixel_alpha);
        }
    }

    music_progress_image_.header.magic = C_ARRAY_HEADER_MAGIC;
    music_progress_image_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    music_progress_image_.header.flags = 0;
    music_progress_image_.header.w = image_width;
    music_progress_image_.header.h = image_height;
    music_progress_image_.header.stride = image_width * sizeof(uint16_t);
    music_progress_image_.header.reserved = 0;
    music_progress_image_.data_size = music_progress_image_data_.size();
    music_progress_image_.data = music_progress_image_data_.data();
    return !music_progress_pixels_.empty();
}

void EmoteDisplay::SetMusicLyrics(const char* title, const char* lyrics)
{
    if (!emote_handle_ || !EnsureMusicUi()) {
        return;
    }

    // EVT_SPEAK hides the battery and replaces it with the speaker icon. Keep
    // music on an independent overlay and explicitly restore the idle status bar.
    UpdateStatusBar();
    emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, nullptr);

    std::array<int, kMusicLyricRowCount> row_widths = {};
    for (size_t i = 0; i < kMusicLyricRowCount; ++i) {
        row_widths[i] = std::max(1, std::min(width_ - 16, width_ * kMusicLyricRowWidth[i] / 360));
    }
    bool truncated = false;
    const auto visual_rows = BuildCircularMusicLyricRows(lyrics, row_widths, &truncated);
    (void)truncated;

    emote_lock(emote_handle_);
    gfx_label_set_text(music_title_, title ? title : "");
    gfx_obj_set_visible(music_background_, true);
    gfx_obj_set_visible(music_title_, true);
    for (size_t i = 0; i < kMusicLyricRowCount; ++i) {
        const bool has_text = i < visual_rows.size() && !visual_rows[i].text.empty();
        gfx_label_set_text(music_lyric_rows_[i], has_text ? visual_rows[i].text.c_str() : "");
        gfx_label_set_color(music_lyric_rows_[i],
                            i < visual_rows.size() && visual_rows[i].current
                                ? GFX_COLOR_HEX(0x62D9FF)
                                : GFX_COLOR_HEX(0xFFFFFF));
        gfx_obj_set_visible(music_lyric_rows_[i], has_text);
    }
    emote_unlock(emote_handle_);

}

void EmoteDisplay::SetMusicProgress(int progress_permille)
{
    if (!emote_handle_ || !EnsureMusicUi()) {
        return;
    }

    const uint16_t progress = static_cast<uint16_t>(std::clamp(progress_permille, 0, 1000));
    emote_lock(emote_handle_);
    auto* colors = reinterpret_cast<uint16_t*>(music_progress_image_data_.data());
    auto* alpha = music_progress_image_data_.data() +
        static_cast<size_t>(music_progress_image_.header.w) *
        music_progress_image_.header.h * sizeof(uint16_t);
    for (size_t i = 0; i < music_progress_pixels_.size(); ++i) {
        const bool played = music_progress_thresholds_[i] <= progress;
        const uint32_t pixel = music_progress_pixels_[i];
        colors[pixel] = played ? music_progress_active_color_ : music_progress_track_color_;
        alpha[pixel] = played
            ? music_progress_pixel_alphas_[i]
            : static_cast<uint8_t>(
                (static_cast<uint16_t>(music_progress_pixel_alphas_[i]) * 72U + 127U) / 255U);
    }
    gfx_img_set_src(music_progress_, &music_progress_image_);
    if (!music_progress_visible_) {
        gfx_obj_set_visible(music_progress_, true);
        music_progress_visible_ = true;
    }
    emote_unlock(emote_handle_);
}

void EmoteDisplay::ClearMusicLyrics()
{
    if (!emote_handle_ || !music_background_) {
        return;
    }
    emote_lock(emote_handle_);
    gfx_obj_set_visible(music_background_, false);
    gfx_obj_set_visible(music_title_, false);
    for (auto* row : music_lyric_rows_) {
        if (row) {
            gfx_obj_set_visible(row, false);
        }
    }
    if (music_progress_) {
        gfx_obj_set_visible(music_progress_, false);
        music_progress_visible_ = false;
    }
    emote_unlock(emote_handle_);
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);
}

bool EmoteDisplay::Lock(const int timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock()
{
}

bool EmoteDisplay::StopAnimDialog()
{
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_);
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms);
    }
    return false;
}

void EmoteDisplay::RefreshAll()
{
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

} // namespace emote
