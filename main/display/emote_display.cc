#include "emote_display.h"

// Standard C++ headers
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
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
#include <sys/stat.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_jpeg_common.h>
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
#include "settings.h"
#include "boards/common/sd_card_manager.h"
#include "jpeg_to_image.h"

extern "C" {
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_30_4);
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
static constexpr int kMusicTimeY = 319;
static constexpr int kMusicTimeWidth = 122;
static constexpr int kMusicTimeHeight = 18;
static constexpr int kMusicRhythmBottomY = 331;
static constexpr int kMusicRhythmBarWidth = 4;
static constexpr std::array<int, 3> kMusicRhythmLeftX = {107, 112, 117};
static constexpr std::array<uint32_t, 3> kMusicRhythmColors = {
    0x2F7185, 0x46A9C7, 0x62D9FF,
};
static constexpr int64_t kWallpaperIdleDelayUs = 30LL * 1000 * 1000;
static constexpr int64_t kWallpaperRotateUs = 30LL * 1000 * 1000;
static constexpr int64_t kWallpaperFadeUs = 220LL * 1000;
static constexpr std::array<const char*, 3> kWallpaperAssets = {
    "wallpaper_dawn.bin",
    "wallpaper_day.bin",
    "wallpaper_night.bin",
};
static constexpr char kWallpaperDirectory[] = "/wallpapers";
static constexpr size_t kMaxCustomWallpapers = 64;
static constexpr off_t kMaxCustomWallpaperBytes = 2 * 1024 * 1024;
static constexpr uint16_t kMaxCustomWallpaperDimension = 1024;
static constexpr int kTouchSurfaceWidth = 360;
static constexpr int kTouchSurfaceHeight = 360;
static constexpr int kTouchSliderVisualWidth = 280;
static constexpr int kTouchSliderVisualHeight = 40;

static uint16_t TouchUiColor(uint32_t color)
{
    return __builtin_bswap16(gfx_color_hex(color).full);
}

static uint32_t MixTouchUiColor(uint32_t first, uint32_t second, int amount)
{
    amount = std::clamp(amount, 0, 255);
    const int inverse = 255 - amount;
    const uint32_t red = (((first >> 16) & 0xFF) * inverse + ((second >> 16) & 0xFF) * amount) / 255;
    const uint32_t green = (((first >> 8) & 0xFF) * inverse + ((second >> 8) & 0xFF) * amount) / 255;
    const uint32_t blue = ((first & 0xFF) * inverse + (second & 0xFF) * amount) / 255;
    return (red << 16) | (green << 8) | blue;
}

static bool TouchUiPointInRoundedRect(int px, int py, int x, int y, int width, int height, int radius)
{
    if (px < x || py < y || px >= x + width || py >= y + height) {
        return false;
    }
    radius = std::max(0, std::min(radius, std::min(width, height) / 2));
    if (radius == 0 || (px >= x + radius && px < x + width - radius) ||
        (py >= y + radius && py < y + height - radius)) {
        return true;
    }
    const int center_x = px < x + radius ? x + radius : x + width - radius - 1;
    const int center_y = py < y + radius ? y + radius : y + height - radius - 1;
    const int dx = px - center_x;
    const int dy = py - center_y;
    return dx * dx + dy * dy <= radius * radius;
}

static void FillTouchUiRoundedRect(std::vector<uint8_t>& data, int image_width, int image_height,
                                   int x, int y, int width, int height, int radius,
                                   uint32_t color, uint8_t alpha = 255)
{
    const size_t pixel_count = static_cast<size_t>(image_width) * image_height;
    auto* colors = reinterpret_cast<uint16_t*>(data.data());
    auto* alphas = data.data() + pixel_count * sizeof(uint16_t);
    const uint16_t converted_color = TouchUiColor(color);
    for (int py = std::max(0, y); py < std::min(image_height, y + height); ++py) {
        for (int px = std::max(0, x); px < std::min(image_width, x + width); ++px) {
            if (TouchUiPointInRoundedRect(px, py, x, y, width, height, radius)) {
                const size_t pixel = static_cast<size_t>(py) * image_width + px;
                colors[pixel] = converted_color;
                alphas[pixel] = alpha;
            }
        }
    }
}

static void FillTouchUiCircle(std::vector<uint8_t>& data, int image_width, int image_height,
                              int center_x, int center_y, int radius, uint32_t color,
                              uint8_t alpha = 255)
{
    const int radius_squared = radius * radius;
    const size_t pixel_count = static_cast<size_t>(image_width) * image_height;
    auto* colors = reinterpret_cast<uint16_t*>(data.data());
    auto* alphas = data.data() + pixel_count * sizeof(uint16_t);
    const uint16_t converted_color = TouchUiColor(color);
    for (int y = center_y - radius; y <= center_y + radius; ++y) {
        for (int x = center_x - radius; x <= center_x + radius; ++x) {
            const int dx = x - center_x;
            const int dy = y - center_y;
            if (x >= 0 && y >= 0 && x < image_width && y < image_height &&
                dx * dx + dy * dy <= radius_squared) {
                const size_t pixel = static_cast<size_t>(y) * image_width + x;
                colors[pixel] = converted_color;
                alphas[pixel] = alpha;
            }
        }
    }
}

static bool HasJpegExtension(const char* name)
{
    if (name == nullptr) {
        return false;
    }
    const std::string filename(name);
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = filename.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".jpg" || extension == ".jpeg";
}

static bool ReadJpegDimensions(FILE* file, uint16_t& width, uint16_t& height)
{
    width = 0;
    height = 0;
    if (file == nullptr || std::fgetc(file) != 0xFF || std::fgetc(file) != 0xD8) {
        return false;
    }
    while (true) {
        int marker_prefix = std::fgetc(file);
        while (marker_prefix == 0xFF) {
            marker_prefix = std::fgetc(file);
        }
        if (marker_prefix == EOF || marker_prefix == 0xD9 || marker_prefix == 0xDA) {
            return false;
        }
        const int marker = marker_prefix;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        const int length_hi = std::fgetc(file);
        const int length_lo = std::fgetc(file);
        if (length_hi == EOF || length_lo == EOF) {
            return false;
        }
        const int segment_length = (length_hi << 8) | length_lo;
        if (segment_length < 2) {
            return false;
        }
        const bool is_sof = (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                             marker != 0xC8 && marker != 0xCC);
        if (is_sof) {
            if (segment_length < 8) {
                return false;
            }
            if (std::fgetc(file) == EOF) {
                return false;
            }
            const int height_hi = std::fgetc(file);
            const int height_lo = std::fgetc(file);
            const int width_hi = std::fgetc(file);
            const int width_lo = std::fgetc(file);
            if (height_hi == EOF || height_lo == EOF || width_hi == EOF || width_lo == EOF) {
                return false;
            }
            height = static_cast<uint16_t>((height_hi << 8) | height_lo);
            width = static_cast<uint16_t>((width_hi << 8) | width_lo);
            return width > 0 && height > 0;
        }
        if (std::fseek(file, segment_length - 2, SEEK_CUR) != 0) {
            return false;
        }
    }
}

static bool ParseAssistantWeather(const char* content, std::string& city,
                                  std::string& condition, int& temperature_c)
{
    const std::string text = content ? content : "";
    const size_t now = text.find("现在");
    if (now == std::string::npos || now == 0) {
        return false;
    }

    const size_t degree = text.find("度", now + strlen("现在"));
    if (degree == std::string::npos) {
        return false;
    }
    size_t number_end = degree;
    while (number_end > now && text[number_end - 1] == ' ') {
        --number_end;
    }
    size_t number_begin = number_end;
    while (number_begin > now && text[number_begin - 1] >= '0' && text[number_begin - 1] <= '9') {
        --number_begin;
    }
    if (number_begin == number_end) {
        return false;
    }

    const size_t first_comma = text.find("，", degree + strlen("度"));
    if (first_comma == std::string::npos) {
        return false;
    }
    const size_t condition_begin = first_comma + strlen("，");
    const size_t condition_end = text.find_first_of("，。；", condition_begin);
    city = text.substr(0, now);
    condition = text.substr(condition_begin, condition_end - condition_begin);
    if (city.empty() || condition.empty()) {
        return false;
    }

    temperature_c = std::atoi(text.substr(number_begin, number_end - number_begin).c_str());
    return temperature_c >= -100 && temperature_c <= 100;
}

static std::string FormatMusicPlaybackTime(int elapsed_seconds, int total_seconds)
{
    elapsed_seconds = std::max(0, elapsed_seconds);
    char text[24] = {};
    if (total_seconds <= 0) {
        snprintf(text, sizeof(text), "%d:%02d/--:--",
                 elapsed_seconds / 60, elapsed_seconds % 60);
    } else if (total_seconds < 3600) {
        elapsed_seconds = std::min(elapsed_seconds, total_seconds);
        snprintf(text, sizeof(text), "%d:%02d/%d:%02d",
                 elapsed_seconds / 60, elapsed_seconds % 60,
                 total_seconds / 60, total_seconds % 60);
    } else {
        elapsed_seconds = std::min(elapsed_seconds, total_seconds);
        snprintf(text, sizeof(text), "%d:%02d/%d:%02d",
                 elapsed_seconds / 3600, (elapsed_seconds % 3600) / 60,
                 total_seconds / 3600, (total_seconds % 3600) / 60);
    }
    return text;
}

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

static std::string TruncateTouchText(const std::string& text, int width_limit)
{
    int total_width = 0;
    size_t scan_position = 0;
    while (scan_position < text.size()) {
        size_t byte_count = 0;
        total_width += EstimateMusicGlyphWidth(
            DecodeUtf8Codepoint(text, scan_position, byte_count));
    }
    if (total_width <= width_limit) {
        return text;
    }

    int width = 0;
    size_t position = 0;
    while (position < text.size()) {
        const size_t character_start = position;
        size_t byte_count = 0;
        const uint32_t codepoint = DecodeUtf8Codepoint(text, position, byte_count);
        const int glyph_width = EstimateMusicGlyphWidth(codepoint);
        if (width + glyph_width + 20 > width_limit) {
            return text.substr(0, character_start) + "…";
        }
        width += glyph_width;
    }
    return text;
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

    if (emote_handle_) {
        wallpaper_image_ = emote_create_obj_by_type(
            emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "wallpaper_background");
        wallpaper_fade_ = emote_create_obj_by_type(
            emote_handle_, EMOTE_OBJ_TYPE_LABEL, "wallpaper_fade");
        if (wallpaper_image_) {
            gfx_obj_set_pos(wallpaper_image_, 0, 0);
            gfx_obj_set_visible(wallpaper_image_, false);
        }
        if (wallpaper_fade_) {
            gfx_obj_set_size(wallpaper_fade_, width_, height_);
            gfx_obj_set_pos(wallpaper_fade_, 0, 0);
            gfx_label_set_text(wallpaper_fade_, "");
            gfx_label_set_bg_color(wallpaper_fade_, GFX_COLOR_HEX(0x000000));
            gfx_label_set_bg_enable(wallpaper_fade_, true);
            gfx_label_set_opa(wallpaper_fade_, 0);
            gfx_obj_set_visible(wallpaper_fade_, false);
        }
    }
    LoadWallpaperSettings();
    const esp_timer_create_args_t timer_args = {
        .callback = &EmoteDisplay::WallpaperTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wallpaper",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &wallpaper_timer_) == ESP_OK) {
        esp_timer_start_periodic(wallpaper_timer_, 250 * 1000);
    }
}

EmoteDisplay::~EmoteDisplay()
{
    if (wallpaper_timer_) {
        esp_timer_stop(wallpaper_timer_);
        esp_timer_delete(wallpaper_timer_);
        wallpaper_timer_ = nullptr;
    }
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
    if (music_font_owned_ && music_font_) {
        gfx_font_lv_delete(static_cast<lv_font_t*>(music_font_));
        music_font_ = nullptr;
        music_font_owned_ = false;
    }
    if (wallpaper_font_owned_ && wallpaper_font_) {
        gfx_font_lv_delete(static_cast<lv_font_t*>(wallpaper_font_));
        wallpaper_font_ = nullptr;
        wallpaper_font_owned_ = false;
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (media_transfer_qr_visible_) {
        return;
    }
    if (emote_handle_ && emotion && strlen(emotion) > 0) {
        HideWallpaper();
        wallpaper_suppressed_until_us_ = esp_timer_get_time() + kWallpaperIdleDelayUs;
        emote_set_anim_emoji(emote_handle_, emotion);
    }
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (media_transfer_qr_visible_) {
        return;
    }
    if (emote_handle_ && content && strlen(content) > 0) {
        if (role && std::strcmp(role, "assistant") == 0) {
            CacheWeatherFromAssistantMessage(content);
        }
        HideWallpaper();
        wallpaper_suppressed_until_us_ = esp_timer_get_time() + kWallpaperIdleDelayUs;
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
    if (media_transfer_qr_visible_) {
        return;
    }
    if (emote_handle_ && status && strlen(status) > 0) {
        if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            wallpaper_idle_ = false;
            HideWallpaper();
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_LISTEN, NULL);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            wallpaper_idle_ = true;
            wallpaper_idle_since_us_ = esp_timer_get_time();
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
        } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            wallpaper_idle_ = false;
            HideWallpaper();
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SPEAK, NULL);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            wallpaper_idle_ = false;
            HideWallpaper();
            emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_SET, NULL);
        }
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (media_transfer_qr_visible_) {
        return;
    }
    if (emote_handle_ && notification && strlen(notification) > 0) {
        HideWallpaper();
        wallpaper_suppressed_until_us_ = esp_timer_get_time() +
            static_cast<int64_t>(std::max(0, duration_ms)) * 1000 + kWallpaperIdleDelayUs;
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
    if (music_background_ && music_title_ && lyric_rows_ready && music_time_ &&
        music_rhythm_ && music_progress_) {
        return true;
    }

    music_background_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "music_background");
    music_title_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "music_title");
    for (size_t i = 0; i < kMusicLyricRowCount; ++i) {
        const std::string name = "music_lyric_" + std::to_string(i);
        music_lyric_rows_[i] = emote_create_obj_by_type(
            emote_handle_, EMOTE_OBJ_TYPE_LABEL, name.c_str());
    }
    // The renderer paints objects in creation order. Create the full-screen
    // transparent progress image first so the compact time and rhythm controls
    // are guaranteed to stay above it.
    music_progress_ = emote_create_obj_by_type(
        emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "music_progress");
    music_time_ = emote_create_obj_by_type(
        emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "music_time");
    music_rhythm_ = emote_create_obj_by_type(
        emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "music_rhythm");
    lyric_rows_ready = true;
    for (auto* row : music_lyric_rows_) {
        lyric_rows_ready = lyric_rows_ready && row != nullptr;
    }
    if (!music_background_ || !music_title_ || !lyric_rows_ready || !music_time_ ||
        !music_rhythm_ || !music_progress_ || !InitializeMusicTimeImage() ||
        !InitializeMusicRhythmImage() || !InitializeMusicProgressImage()) {
        ESP_LOGE(TAG, "Failed to create music lyric UI");
        return false;
    }

    music_font_ = EnsureCommonTextFont();

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

    const int time_y = height_ * kMusicTimeY / 360;
    RenderMusicTimeImage("0:00/--:--");
    gfx_img_set_src(music_time_, &music_time_image_);
    gfx_obj_set_pos(music_time_, (width_ - kMusicTimeWidth) / 2, time_y);

    const int bar_bottom = height_ * kMusicRhythmBottomY / 360;
    RenderMusicRhythmImage(0, 0, 0);
    gfx_img_set_src(music_rhythm_, &music_rhythm_image_);
    gfx_obj_set_pos(music_rhythm_, 0, bar_bottom - music_rhythm_image_.header.h);

    gfx_obj_set_visible(music_background_, false);
    gfx_obj_set_visible(music_title_, false);
    for (auto* row : music_lyric_rows_) {
        gfx_obj_set_visible(row, false);
    }
    gfx_obj_set_visible(music_time_, false);
    gfx_obj_set_visible(music_rhythm_, false);
    gfx_img_set_src(music_progress_, &music_progress_image_);
    gfx_obj_set_pos(music_progress_, 0, height_ / 2 - 5);
    gfx_obj_set_visible(music_progress_, false);
    music_progress_visible_ = false;
    emote_unlock(emote_handle_);
    return true;
}

gfx_font_t EmoteDisplay::EnsureCommonTextFont()
{
    if (music_font_) {
        return music_font_;
    }
    music_font_ = LoadMusicFontFromAssets();
    if (music_font_) {
        music_font_owned_ = true;
    } else {
        music_font_ = static_cast<gfx_font_t>(
            const_cast<lv_font_t*>(&font_puhui_basic_20_4));
        ESP_LOGW(TAG, "Emote common text font unavailable; using basic fallback");
    }
    return music_font_;
}

bool EmoteDisplay::InitializeMusicTimeImage()
{
    if (!music_time_image_data_.empty()) {
        return true;
    }

    const size_t pixel_count = static_cast<size_t>(kMusicTimeWidth) * kMusicTimeHeight;
    music_time_image_data_.assign(pixel_count * 3, 0);
    music_time_image_.header.magic = C_ARRAY_HEADER_MAGIC;
    music_time_image_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    music_time_image_.header.flags = 0;
    music_time_image_.header.w = kMusicTimeWidth;
    music_time_image_.header.h = kMusicTimeHeight;
    music_time_image_.header.stride = kMusicTimeWidth * sizeof(uint16_t);
    music_time_image_.header.reserved = 0;
    music_time_image_.data_size = music_time_image_data_.size();
    music_time_image_.data = music_time_image_data_.data();
    return true;
}

void EmoteDisplay::RenderMusicTimeImage(const std::string& text)
{
    if (!InitializeMusicTimeImage()) {
        return;
    }

    static constexpr uint8_t kDigits[10][7] = {
        {0xF, 0x9, 0x9, 0x9, 0x9, 0x9, 0xF},
        {0x6, 0x2, 0x2, 0x2, 0x2, 0x2, 0x7},
        {0xF, 0x1, 0x1, 0xF, 0x8, 0x8, 0xF},
        {0xF, 0x1, 0x1, 0x7, 0x1, 0x1, 0xF},
        {0x9, 0x9, 0x9, 0xF, 0x1, 0x1, 0x1},
        {0xF, 0x8, 0x8, 0xF, 0x1, 0x1, 0xF},
        {0xF, 0x8, 0x8, 0xF, 0x9, 0x9, 0xF},
        {0xF, 0x1, 0x1, 0x2, 0x2, 0x4, 0x4},
        {0xF, 0x9, 0x9, 0xF, 0x9, 0x9, 0xF},
        {0xF, 0x9, 0x9, 0xF, 0x1, 0x1, 0xF},
    };
    static constexpr uint8_t kColon[7] = {0, 0, 0x2, 0, 0, 0x2, 0};
    static constexpr uint8_t kSlash[7] = {0x1, 0x1, 0x2, 0x2, 0x4, 0x4, 0x8};
    static constexpr uint8_t kHyphen[7] = {0, 0, 0, 0xF, 0, 0, 0};
    static constexpr int kScale = 2;
    static constexpr int kGap = 2;

    auto glyph_width = [](char character) {
        return character == ':' ? 2 : 4;
    };

    int total_width = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        total_width += glyph_width(text[i]) * kScale;
        if (i + 1 < text.size()) {
            total_width += kGap;
        }
    }

    std::fill(music_time_image_data_.begin(), music_time_image_data_.end(), 0);
    const size_t pixel_count = static_cast<size_t>(kMusicTimeWidth) * kMusicTimeHeight;
    auto* colors = reinterpret_cast<uint16_t*>(music_time_image_data_.data());
    auto* alpha = music_time_image_data_.data() + pixel_count * sizeof(uint16_t);
    const uint16_t elapsed_color = __builtin_bswap16(gfx_color_hex(0x62D9FF).full);
    const uint16_t total_color = __builtin_bswap16(gfx_color_hex(0xD7DCE2).full);
    const size_t slash_position = text.find('/');
    int cursor_x = std::max(0, (kMusicTimeWidth - total_width) / 2);
    constexpr int start_y = (kMusicTimeHeight - 7 * kScale) / 2;

    for (size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        const uint8_t* rows = nullptr;
        if (character >= '0' && character <= '9') {
            rows = kDigits[character - '0'];
        } else if (character == ':') {
            rows = kColon;
        } else if (character == '/') {
            rows = kSlash;
        } else if (character == '-') {
            rows = kHyphen;
        }

        const int width = glyph_width(character);
        const uint16_t color = slash_position == std::string::npos || index < slash_position
            ? elapsed_color
            : total_color;
        if (rows != nullptr) {
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < width; ++column) {
                    if ((rows[row] & (1U << (width - 1 - column))) == 0) {
                        continue;
                    }
                    for (int y = 0; y < kScale; ++y) {
                        for (int x = 0; x < kScale; ++x) {
                            const int pixel_x = cursor_x + column * kScale + x;
                            const int pixel_y = start_y + row * kScale + y;
                            if (pixel_x < 0 || pixel_x >= kMusicTimeWidth ||
                                pixel_y < 0 || pixel_y >= kMusicTimeHeight) {
                                continue;
                            }
                            const size_t pixel = static_cast<size_t>(pixel_y) *
                                kMusicTimeWidth + pixel_x;
                            colors[pixel] = color;
                            alpha[pixel] = 255;
                        }
                    }
                }
            }
        }
        cursor_x += width * kScale + kGap;
    }
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
    // Image pixels are copied directly into the byte-swapped panel buffer when
    // alpha is fully opaque, so store RGB565 in panel byte order.
    music_progress_track_color_ = __builtin_bswap16(gfx_color_hex(0xD7DCE2).full);
    music_progress_active_color_ = __builtin_bswap16(gfx_color_hex(0x62D9FF).full);

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

    wallpaper_music_active_ = true;
    HideWallpaper();

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
    const bool show_overlay = !touch_music_overlay_suppressed_;
    gfx_obj_set_visible(music_background_, show_overlay);
    gfx_obj_set_visible(music_title_, show_overlay);
    for (size_t i = 0; i < kMusicLyricRowCount; ++i) {
        const bool has_text = i < visual_rows.size() && !visual_rows[i].text.empty();
        gfx_label_set_text(music_lyric_rows_[i], has_text ? visual_rows[i].text.c_str() : "");
        gfx_label_set_color(music_lyric_rows_[i],
                            i < visual_rows.size() && visual_rows[i].current
                                ? GFX_COLOR_HEX(0x62D9FF)
                                : GFX_COLOR_HEX(0xFFFFFF));
        gfx_obj_set_visible(music_lyric_rows_[i], has_text && show_overlay);
    }
    if (!music_playback_info_visible_) {
        RenderMusicTimeImage("0:00/--:--");
        gfx_img_set_src(music_time_, &music_time_image_);
        gfx_obj_set_visible(music_time_, show_overlay);
        gfx_obj_set_visible(music_rhythm_, show_overlay);
        music_playback_info_visible_ = true;
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
    music_progress_visible_ = true;
    gfx_obj_set_visible(music_progress_, !touch_music_overlay_suppressed_);
    emote_unlock(emote_handle_);
}

void EmoteDisplay::SetMusicPlaybackInfo(int elapsed_seconds, int total_seconds,
                                        int level_0, int level_1, int level_2)
{
    if (!emote_handle_ || !EnsureMusicUi()) {
        return;
    }

    const int levels[3] = {
        std::clamp(level_0, 0, 1000),
        std::clamp(level_1, 0, 1000),
        std::clamp(level_2, 0, 1000),
    };
    emote_lock(emote_handle_);
    if (elapsed_seconds != music_elapsed_seconds_ || total_seconds != music_total_seconds_) {
        const std::string time = FormatMusicPlaybackTime(elapsed_seconds, total_seconds);
        RenderMusicTimeImage(time);
        gfx_img_set_src(music_time_, &music_time_image_);
        music_elapsed_seconds_ = elapsed_seconds;
        music_total_seconds_ = total_seconds;
    }
    if (!std::equal(std::begin(levels), std::end(levels), std::begin(music_rhythm_heights_))) {
        RenderMusicRhythmImage(levels[0], levels[1], levels[2]);
        gfx_img_set_src(music_rhythm_, &music_rhythm_image_);
        std::copy(std::begin(levels), std::end(levels), std::begin(music_rhythm_heights_));
    }
    if (!music_playback_info_visible_) {
        music_playback_info_visible_ = true;
    }
    gfx_obj_set_visible(music_time_, !touch_music_overlay_suppressed_);
    gfx_obj_set_visible(music_rhythm_, !touch_music_overlay_suppressed_);
    emote_unlock(emote_handle_);
}

bool EmoteDisplay::InitializeMusicRhythmImage()
{
    if (!music_rhythm_image_data_.empty()) return true;
    constexpr int image_height = 12;
    const size_t pixel_count = static_cast<size_t>(width_) * image_height;
    music_rhythm_image_data_.assign(pixel_count * 3, 0);
    music_rhythm_image_.header.magic = C_ARRAY_HEADER_MAGIC;
    music_rhythm_image_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    music_rhythm_image_.header.flags = 0;
    music_rhythm_image_.header.w = width_;
    music_rhythm_image_.header.h = image_height;
    music_rhythm_image_.header.stride = width_ * sizeof(uint16_t);
    music_rhythm_image_.header.reserved = 0;
    music_rhythm_image_.data_size = music_rhythm_image_data_.size();
    music_rhythm_image_.data = music_rhythm_image_data_.data();
    return true;
}

void EmoteDisplay::RenderMusicRhythmImage(int level_0, int level_1, int level_2)
{
    if (!InitializeMusicRhythmImage()) return;
    const int levels[3] = {std::clamp(level_0, 0, 1000),
                           std::clamp(level_1, 0, 1000),
                           std::clamp(level_2, 0, 1000)};
    std::fill(music_rhythm_image_data_.begin(), music_rhythm_image_data_.end(), 0);
    const size_t pixel_count = static_cast<size_t>(music_rhythm_image_.header.w) *
                               music_rhythm_image_.header.h;
    auto* colors = reinterpret_cast<uint16_t*>(music_rhythm_image_data_.data());
    auto* alpha = music_rhythm_image_data_.data() + pixel_count * sizeof(uint16_t);
    const int bar_width = std::max(2, width_ * kMusicRhythmBarWidth / 360);
    for (size_t i = 0; i < 3; ++i) {
        const int height = 1 + levels[i] * 9 / 1000;
        const int left_x = width_ * kMusicRhythmLeftX[i] / 360;
        const int positions[2] = {left_x, width_ - left_x - bar_width};
        const uint16_t color = __builtin_bswap16(gfx_color_hex(kMusicRhythmColors[i]).full);
        for (int x0 : positions) {
            for (int y = music_rhythm_image_.header.h - height;
                 y < music_rhythm_image_.header.h; ++y) {
                for (int x = x0; x < x0 + bar_width; ++x) {
                    const size_t pixel = static_cast<size_t>(y) * width_ + x;
                    colors[pixel] = color;
                    alpha[pixel] = 255;
                }
            }
        }
    }
}

void EmoteDisplay::SetMusicOverlayVisible(bool visible)
{
    if (!emote_handle_ || !music_background_) {
        return;
    }
    const bool show = visible && wallpaper_music_active_;
    emote_lock(emote_handle_);
    gfx_obj_set_visible(music_background_, show);
    gfx_obj_set_visible(music_title_, show);
    for (auto* row : music_lyric_rows_) {
        if (row) gfx_obj_set_visible(row, show);
    }
    if (music_time_) gfx_obj_set_visible(music_time_, show && music_playback_info_visible_);
    if (music_rhythm_) gfx_obj_set_visible(music_rhythm_, show && music_playback_info_visible_);
    if (music_progress_) gfx_obj_set_visible(music_progress_, show && music_progress_visible_);
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
    if (music_time_) {
        gfx_obj_set_visible(music_time_, false);
    }
    if (music_rhythm_) gfx_obj_set_visible(music_rhythm_, false);
    music_playback_info_visible_ = false;
    if (music_progress_) {
        gfx_obj_set_visible(music_progress_, false);
        music_progress_visible_ = false;
    }
    emote_unlock(emote_handle_);
    wallpaper_music_active_ = false;
    wallpaper_idle_since_us_ = esp_timer_get_time();
}

void EmoteDisplay::SetWallpaperLocation(const char* city)
{
    if (city == nullptr || city[0] == '\0') {
        return;
    }
    wallpaper_city_ = city;
    SaveWallpaperSettings();
    UpdateWallpaperText(true);
}

void EmoteDisplay::SetWallpaperWeather(const char* city, const char* condition,
                                       int temperature_c, int high_c, int low_c)
{
    if (condition == nullptr || condition[0] == '\0') {
        return;
    }
    if (city != nullptr && city[0] != '\0') {
        wallpaper_city_ = city;
    }
    wallpaper_condition_ = condition;
    wallpaper_temperature_c_ = temperature_c;
    wallpaper_high_c_ = high_c;
    wallpaper_low_c_ = low_c;
    SaveWallpaperSettings();
    UpdateWallpaperText(true);
}

void EmoteDisplay::CacheWeatherFromAssistantMessage(const char* content)
{
    std::string city;
    std::string condition;
    int temperature_c = 0;
    if (!ParseAssistantWeather(content, city, condition, temperature_c)) {
        return;
    }

    ESP_LOGI(TAG, "Cached weather from assistant: %s, %s, %dC",
             city.c_str(), condition.c_str(), temperature_c);
    SetWallpaperWeather(city.c_str(), condition.c_str(), temperature_c,
                        kWallpaperUnsetTemperature, kWallpaperUnsetTemperature);
}

void EmoteDisplay::WallpaperTimerCallback(void* arg)
{
    static_cast<EmoteDisplay*>(arg)->TickWallpaper();
}

bool EmoteDisplay::EnsureWallpaperUi()
{
    if (!emote_handle_ || !wallpaper_image_ || !wallpaper_fade_) {
        return false;
    }
    if (wallpaper_date_ && wallpaper_time_ && wallpaper_weather_) {
        return true;
    }

    wallpaper_date_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "wallpaper_date");
    wallpaper_time_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "wallpaper_time");
    wallpaper_weather_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "wallpaper_weather");
    if (!wallpaper_date_ || !wallpaper_time_ || !wallpaper_weather_) {
        ESP_LOGE(TAG, "Failed to create wallpaper UI");
        return false;
    }

    wallpaper_font_ = LoadMusicFontFromAssets();
    if (wallpaper_font_) {
        wallpaper_font_owned_ = true;
    } else {
        wallpaper_font_ = static_cast<gfx_font_t>(const_cast<lv_font_t*>(&font_puhui_basic_20_4));
    }

    emote_lock(emote_handle_);
    gfx_obj_set_size(wallpaper_date_, std::max(1, width_ - 56), 30);
    gfx_obj_align(wallpaper_date_, GFX_ALIGN_TOP_MID, 0, 72);
    gfx_label_set_font(wallpaper_date_, wallpaper_font_);
    gfx_label_set_color(wallpaper_date_, GFX_COLOR_HEX(0xF5F7FA));
    gfx_label_set_text_align(wallpaper_date_, GFX_TEXT_ALIGN_CENTER);

    gfx_obj_set_size(wallpaper_time_, std::max(1, width_ - 72), 60);
    gfx_obj_align(wallpaper_time_, GFX_ALIGN_TOP_MID, 0, 108);
    gfx_label_set_font(wallpaper_time_, static_cast<gfx_font_t>(const_cast<lv_font_t*>(&font_puhui_basic_30_4)));
    gfx_label_set_color(wallpaper_time_, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(wallpaper_time_, GFX_TEXT_ALIGN_CENTER);

    gfx_obj_set_size(wallpaper_weather_, std::max(1, width_ - 52), 60);
    gfx_obj_align(wallpaper_weather_, GFX_ALIGN_TOP_MID, 0, 218);
    gfx_label_set_font(wallpaper_weather_, wallpaper_font_);
    gfx_label_set_color(wallpaper_weather_, GFX_COLOR_HEX(0xF5F7FA));
    gfx_label_set_text_align(wallpaper_weather_, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(wallpaper_weather_, GFX_LABEL_LONG_WRAP);
    gfx_label_set_line_spacing(wallpaper_weather_, 4);

    gfx_obj_set_visible(wallpaper_date_, false);
    gfx_obj_set_visible(wallpaper_time_, false);
    gfx_obj_set_visible(wallpaper_weather_, false);
    emote_unlock(emote_handle_);
    return true;
}

bool EmoteDisplay::LoadWallpaperAsset(int index)
{
    if (index < 0 || index >= static_cast<int>(kWallpaperAssets.size())) {
        return false;
    }
    void* data = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(kWallpaperAssets[index], data, size) ||
        data == nullptr || size <= sizeof(gfx_image_header_t)) {
        ESP_LOGW(TAG, "Unable to load wallpaper asset %s", kWallpaperAssets[index]);
        return false;
    }
    std::memcpy(&wallpaper_image_dsc_.header, data, sizeof(gfx_image_header_t));
    wallpaper_image_dsc_.data_size = static_cast<uint32_t>(size - sizeof(gfx_image_header_t));
    wallpaper_image_dsc_.data = static_cast<const uint8_t*>(data) + sizeof(gfx_image_header_t);
    wallpaper_image_dsc_.reserved = nullptr;
    wallpaper_image_dsc_.reserved_2 = nullptr;
    return wallpaper_image_dsc_.header.magic == C_ARRAY_HEADER_MAGIC &&
        wallpaper_image_dsc_.header.w == width_ && wallpaper_image_dsc_.header.h == height_;
}

size_t EmoteDisplay::WallpaperCount()
{
    std::lock_guard<std::mutex> lock(wallpaper_data_mutex_);
    return kWallpaperAssets.size() + custom_wallpaper_paths_.size();
}

bool EmoteDisplay::ScanCustomWallpapers(std::string& result)
{
    auto& sd_card = SdCardManager::GetInstance();
    const std::string mount_error = sd_card.EnsureMounted();
    if (!mount_error.empty()) {
        result = mount_error;
        return false;
    }

    std::vector<std::string> paths;
    const std::string directory = std::string(sd_card.MountPoint()) + kWallpaperDirectory;
    std::lock_guard<std::mutex> filesystem_lock(sd_card.FilesystemMutex());
    if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) {
        result = "Unable to create /wallpapers on the SD card.";
        ESP_LOGW(TAG, "%s: %s", result.c_str(), strerror(errno));
        return false;
    }

    DIR* folder = opendir(directory.c_str());
    if (folder == nullptr) {
        result = "Unable to open /wallpapers on the SD card.";
        ESP_LOGW(TAG, "%s: %s", result.c_str(), strerror(errno));
        return false;
    }
    while (paths.size() < kMaxCustomWallpapers) {
        dirent* entry = readdir(folder);
        if (entry == nullptr) {
            break;
        }
        if (!HasJpegExtension(entry->d_name)) {
            continue;
        }
        const std::string path = directory + "/" + entry->d_name;
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
            info.st_size > kMaxCustomWallpaperBytes) {
            ESP_LOGW(TAG, "Skipping custom wallpaper %s: invalid size", entry->d_name);
            continue;
        }
        FILE* file = std::fopen(path.c_str(), "rb");
        uint16_t image_width = 0;
        uint16_t image_height = 0;
        const bool valid = file != nullptr && ReadJpegDimensions(file, image_width, image_height);
        if (file != nullptr) {
            std::fclose(file);
        }
        if (!valid || image_width > kMaxCustomWallpaperDimension ||
            image_height > kMaxCustomWallpaperDimension) {
            ESP_LOGW(TAG, "Skipping custom wallpaper %s: invalid JPEG dimensions", entry->d_name);
            continue;
        }
        paths.push_back(path);
    }
    closedir(folder);

    std::sort(paths.begin(), paths.end());
    const size_t custom_count = paths.size();
    {
        std::lock_guard<std::mutex> lock(wallpaper_data_mutex_);
        custom_wallpaper_paths_ = std::move(paths);
        for (auto& buffer : custom_wallpaper_pixels_) buffer.clear();
    }
    result = "Custom wallpaper scan found " + std::to_string(custom_count) + " JPEG file(s).";
    return true;
}

std::string EmoteDisplay::RefreshCustomWallpapers()
{
    HideWallpaper();
    std::string result;
    if (!ScanCustomWallpapers(result)) {
        std::lock_guard<std::mutex> lock(wallpaper_data_mutex_);
        custom_wallpaper_paths_.clear();
        for (auto& buffer : custom_wallpaper_pixels_) buffer.clear();
    }
    const size_t count = WallpaperCount();
    wallpaper_index_ = count == 0 ? 0 : std::clamp(wallpaper_index_, 0, static_cast<int>(count - 1));
    wallpaper_shown_since_us_ = 0;
    ESP_LOGI(TAG, "%s", result.c_str());
    return result;
}

void EmoteDisplay::RequestCustomWallpaperRefresh()
{
    wallpaper_refresh_requested_.store(true, std::memory_order_release);
}

bool EmoteDisplay::LoadCustomWallpaper(int index)
{
    std::string path;
    {
        std::lock_guard<std::mutex> lock(wallpaper_data_mutex_);
        if (index < 0 || index >= static_cast<int>(custom_wallpaper_paths_.size())) {
            return false;
        }
        path = custom_wallpaper_paths_[index];
    }

    auto& sd_card = SdCardManager::GetInstance();
    if (!sd_card.EnsureMounted().empty()) {
        return false;
    }
    std::vector<uint8_t> jpeg;
    {
        std::lock_guard<std::mutex> filesystem_lock(sd_card.FilesystemMutex());
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0 || info.st_size <= 0 ||
            info.st_size > kMaxCustomWallpaperBytes) {
            return false;
        }
        FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            return false;
        }
        jpeg.resize(static_cast<size_t>(info.st_size));
        const size_t read = std::fread(jpeg.data(), 1, jpeg.size(), file);
        std::fclose(file);
        if (read != jpeg.size()) {
            return false;
        }
    }

    uint8_t* decoded = nullptr;
    size_t decoded_size = 0;
    size_t source_width = 0;
    size_t source_height = 0;
    size_t source_stride = 0;
    if (jpeg_to_image(jpeg.data(), jpeg.size(), &decoded, &decoded_size, &source_width,
                      &source_height, &source_stride) != ESP_OK || decoded == nullptr ||
        source_width == 0 || source_height == 0 || source_width > kMaxCustomWallpaperDimension ||
        source_height > kMaxCustomWallpaperDimension ||
        source_stride < source_width * sizeof(uint16_t) ||
        decoded_size < source_stride * source_height) {
        if (decoded != nullptr) {
            jpeg_free_align(decoded);
        }
        return false;
    }

    const float scale = std::max(static_cast<float>(width_) / source_width,
                                 static_cast<float>(height_) / source_height);
    const float crop_width = static_cast<float>(width_) / scale;
    const float crop_height = static_cast<float>(height_) / scale;
    const float crop_x = (static_cast<float>(source_width) - crop_width) / 2.0f;
    const float crop_y = (static_cast<float>(source_height) - crop_height) / 2.0f;
    std::vector<uint8_t> pixels(static_cast<size_t>(width_) * height_ * sizeof(uint16_t));
    for (int y = 0; y < height_; ++y) {
        const size_t source_y = std::min(source_height - 1, static_cast<size_t>(
            std::max(0.0f, crop_y + (static_cast<float>(y) + 0.5f) / scale)));
        for (int x = 0; x < width_; ++x) {
            const size_t source_x = std::min(source_width - 1, static_cast<size_t>(
                std::max(0.0f, crop_x + (static_cast<float>(x) + 0.5f) / scale)));
            const size_t source_offset = source_y * source_stride + source_x * sizeof(uint16_t);
            const size_t target_offset = (static_cast<size_t>(y) * width_ + x) * sizeof(uint16_t);
            pixels[target_offset] = decoded[source_offset + 1];
            pixels[target_offset + 1] = decoded[source_offset];
        }
    }
    jpeg_free_align(decoded);

    std::lock_guard<std::mutex> lock(wallpaper_data_mutex_);
    custom_wallpaper_buffer_index_ = (custom_wallpaper_buffer_index_ + 1) %
                                     custom_wallpaper_pixels_.size();
    auto& active_pixels = custom_wallpaper_pixels_[custom_wallpaper_buffer_index_];
    active_pixels = std::move(pixels);
    wallpaper_image_dsc_.header.magic = C_ARRAY_HEADER_MAGIC;
    wallpaper_image_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565;
    wallpaper_image_dsc_.header.flags = 0;
    wallpaper_image_dsc_.header.w = width_;
    wallpaper_image_dsc_.header.h = height_;
    wallpaper_image_dsc_.header.stride = width_ * sizeof(uint16_t);
    wallpaper_image_dsc_.header.reserved = 0;
    wallpaper_image_dsc_.data_size = active_pixels.size();
    wallpaper_image_dsc_.data = active_pixels.data();
    wallpaper_image_dsc_.reserved = nullptr;
    wallpaper_image_dsc_.reserved_2 = nullptr;
    return true;
}

bool EmoteDisplay::LoadWallpaper(int index)
{
    if (index >= 0 && index < static_cast<int>(kWallpaperAssets.size())) {
        return LoadWallpaperAsset(index);
    }
    const int custom_index = index - static_cast<int>(kWallpaperAssets.size());
    if (LoadCustomWallpaper(custom_index)) {
        return true;
    }
    ESP_LOGW(TAG, "Custom wallpaper %d failed; using an internal wallpaper", index);
    return LoadWallpaperAsset(index >= 0 ? index % kWallpaperAssets.size() : 0);
}

void EmoteDisplay::SetWallpaperNativeUiVisible(bool visible)
{
    if (!emote_handle_) {
        return;
    }

    emote_lock(emote_handle_);
    if (auto* eye = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_EYE_ANIM)) {
        gfx_obj_set_visible(eye, visible);
    }
    if (auto* clock = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_CLOCK_LABEL)) {
        gfx_obj_set_visible(clock, visible);
    }
    if (auto* timer = emote_get_obj_by_name(emote_handle_, EMT_DEF_ELEM_TIMER_STATUS)) {
        if (visible) {
            gfx_timer_resume(static_cast<gfx_timer_handle_t>(timer));
        } else {
            gfx_timer_pause(static_cast<gfx_timer_handle_t>(timer));
        }
    }
    emote_unlock(emote_handle_);
}

void EmoteDisplay::ShowWallpaper()
{
    if (!EnsureWallpaperUi() || !LoadWallpaper(wallpaper_index_)) {
        wallpaper_suppressed_until_us_ = esp_timer_get_time() + kWallpaperIdleDelayUs;
        return;
    }
    wallpaper_visible_ = true;
    wallpaper_shown_since_us_ = esp_timer_get_time();
    wallpaper_fade_until_us_ = wallpaper_shown_since_us_ + kWallpaperFadeUs;
    // The wallpaper background was intentionally created before the native
    // Emote objects so the battery indicator remains above it. Hide only the
    // native face and clock; otherwise they cover the image and duplicate time.
    SetWallpaperNativeUiVisible(false);
    emote_lock(emote_handle_);
    gfx_img_set_src(wallpaper_image_, &wallpaper_image_dsc_);
    gfx_obj_set_visible(wallpaper_image_, true);
    gfx_label_set_opa(wallpaper_fade_, 115);
    gfx_obj_set_visible(wallpaper_fade_, true);
    gfx_obj_set_visible(wallpaper_date_, true);
    gfx_obj_set_visible(wallpaper_time_, true);
    gfx_obj_set_visible(wallpaper_weather_, !wallpaper_condition_.empty());
    emote_unlock(emote_handle_);
    UpdateWallpaperText(true);
    UpdateStatusBar();
}

void EmoteDisplay::HideWallpaper()
{
    if (!wallpaper_visible_ || !emote_handle_) {
        return;
    }
    emote_lock(emote_handle_);
    gfx_obj_set_visible(wallpaper_image_, false);
    gfx_obj_set_visible(wallpaper_fade_, false);
    if (wallpaper_date_) {
        gfx_obj_set_visible(wallpaper_date_, false);
        gfx_obj_set_visible(wallpaper_time_, false);
        gfx_obj_set_visible(wallpaper_weather_, false);
    }
    emote_unlock(emote_handle_);
    wallpaper_visible_ = false;
    wallpaper_last_minute_ = -1;
    SetWallpaperNativeUiVisible(true);
}

bool EmoteDisplay::ShowMediaTransferQr(const char* url)
{
    if (!emote_handle_ || !url) {
        return false;
    }
    HideWallpaper();
    if (!media_transfer_qr_) {
        media_transfer_qr_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_QRCODE,
                                                       "media_transfer_qr");
    }
    if (!media_transfer_info_) {
        media_transfer_info_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL,
                                                         "media_transfer_info");
    }
    auto* qr = media_transfer_qr_;
    if (!qr || !media_transfer_info_) {
        ESP_LOGW(TAG, "Unable to create wallpaper server display objects");
        return false;
    }
    const char* address = std::strstr(url, "//");
    address = address ? address + 2 : url;
    char info[96];
    std::snprintf(info, sizeof(info), "%s", address);
    emote_lock(emote_handle_);
    gfx_qrcode_set_size(qr, 200);
    gfx_qrcode_set_color(qr, GFX_COLOR_HEX(0x000000));
    gfx_qrcode_set_bg_color(qr, GFX_COLOR_HEX(0xFFFFFF));
    gfx_obj_align(qr, GFX_ALIGN_CENTER, 0, -28);
    gfx_obj_set_size(media_transfer_info_, width_ - 24, 68);
    gfx_obj_align(media_transfer_info_, GFX_ALIGN_BOTTOM_MID, 0, -8);
    gfx_label_set_font(media_transfer_info_, static_cast<gfx_font_t>(const_cast<lv_font_t*>(&font_puhui_basic_20_4)));
    gfx_label_set_color(media_transfer_info_, GFX_COLOR_HEX(0xF4F8FF));
    gfx_label_set_text_align(media_transfer_info_, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_text(media_transfer_info_, info);
    emote_unlock(emote_handle_);
    emote_lock(emote_handle_);
    const esp_err_t result = gfx_qrcode_set_data(qr, url);
    if (result == ESP_OK) {
        gfx_obj_set_visible(qr, true);
        gfx_obj_set_visible(media_transfer_info_, true);
    }
    emote_unlock(emote_handle_);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Unable to show media transfer QR: %s", esp_err_to_name(result));
        return false;
    }
    SetWallpaperNativeUiVisible(false);
    media_transfer_qr_visible_ = true;
    return true;
}

void EmoteDisplay::HideMediaTransferQr()
{
    if (!emote_handle_) {
        return;
    }
    emote_lock(emote_handle_);
    if (media_transfer_qr_) {
        gfx_obj_set_visible(media_transfer_qr_, false);
    }
    if (media_transfer_info_) {
        gfx_obj_set_visible(media_transfer_info_, false);
    }
    emote_unlock(emote_handle_);
    media_transfer_qr_visible_ = false;
    SetWallpaperNativeUiVisible(true);
    emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, nullptr);
}

void EmoteDisplay::TickWallpaper()
{
    if (wallpaper_refresh_requested_.exchange(false, std::memory_order_acq_rel)) {
        RefreshCustomWallpapers();
    }
    if (media_transfer_qr_visible_ || touch_settings_visible_) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    const bool eligible = wallpaper_idle_ && !wallpaper_music_active_ &&
        now_us >= wallpaper_suppressed_until_us_ &&
        wallpaper_idle_since_us_ > 0 &&
        now_us - wallpaper_idle_since_us_ >= kWallpaperIdleDelayUs;
    if (!eligible) {
        HideWallpaper();
        return;
    }
    if (!wallpaper_visible_) {
        ShowWallpaper();
        return;
    }
    if (wallpaper_fade_until_us_ > 0 && now_us >= wallpaper_fade_until_us_) {
        emote_lock(emote_handle_);
        gfx_obj_set_visible(wallpaper_fade_, false);
        emote_unlock(emote_handle_);
        wallpaper_fade_until_us_ = 0;
    }
    if (now_us - wallpaper_shown_since_us_ >= kWallpaperRotateUs) {
        const size_t wallpaper_count = WallpaperCount();
        if (wallpaper_count == 0) {
            return;
        }
        wallpaper_index_ = (wallpaper_index_ + 1) % wallpaper_count;
        if (LoadWallpaper(wallpaper_index_)) {
            emote_lock(emote_handle_);
            gfx_label_set_opa(wallpaper_fade_, 115);
            gfx_obj_set_visible(wallpaper_fade_, true);
            gfx_img_set_src(wallpaper_image_, &wallpaper_image_dsc_);
            emote_unlock(emote_handle_);
            wallpaper_shown_since_us_ = now_us;
            wallpaper_fade_until_us_ = now_us + kWallpaperFadeUs;
        }
    }
    UpdateWallpaperText(false);
}

void EmoteDisplay::UpdateWallpaperText(bool force)
{
    if (!wallpaper_visible_ || !wallpaper_date_ || !wallpaper_time_ || !wallpaper_weather_) {
        return;
    }
    time_t now = time(nullptr);
    struct tm local_time = {};
    localtime_r(&now, &local_time);
    const int minute_key = local_time.tm_yday * 24 * 60 + local_time.tm_hour * 60 + local_time.tm_min;
    if (!force && minute_key == wallpaper_last_minute_) {
        return;
    }
    static constexpr std::array<const char*, 7> weekdays = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六",
    };
    char date[40] = {};
    char clock[16] = {};
    snprintf(date, sizeof(date), "%04d-%02d-%02d  %s", local_time.tm_year + 1900,
             local_time.tm_mon + 1, local_time.tm_mday, weekdays[local_time.tm_wday]);
    snprintf(clock, sizeof(clock), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
    std::string weather;
    if (!wallpaper_condition_.empty()) {
        const std::string city = wallpaper_city_.empty() ? "本地" : wallpaper_city_;
        weather = city + " · " + wallpaper_condition_ + " · " +
            std::to_string(wallpaper_temperature_c_) + "°C";
        if (wallpaper_high_c_ != kWallpaperUnsetTemperature &&
            wallpaper_low_c_ != kWallpaperUnsetTemperature) {
            weather += "  " + std::to_string(wallpaper_low_c_) + "~" +
                std::to_string(wallpaper_high_c_) + "°C";
        }
    }
    emote_lock(emote_handle_);
    gfx_label_set_text(wallpaper_date_, date);
    gfx_label_set_text(wallpaper_time_, clock);
    gfx_label_set_text(wallpaper_weather_, weather.c_str());
    gfx_obj_set_visible(wallpaper_weather_, !weather.empty());
    emote_unlock(emote_handle_);
    wallpaper_last_minute_ = minute_key;
}

void EmoteDisplay::LoadWallpaperSettings()
{
    Settings settings("wallpaper", false);
    wallpaper_index_ = std::max(0, static_cast<int>(settings.GetInt("index", 0)));
    wallpaper_city_ = settings.GetString("city");
    if (settings.GetBool("weather_valid", false)) {
        wallpaper_condition_ = settings.GetString("condition");
        wallpaper_temperature_c_ = settings.GetInt("temperature_c", kWallpaperUnsetTemperature);
        wallpaper_high_c_ = settings.GetInt("high_c", kWallpaperUnsetTemperature);
        wallpaper_low_c_ = settings.GetInt("low_c", kWallpaperUnsetTemperature);
    }
}

void EmoteDisplay::SaveWallpaperSettings()
{
    Settings settings("wallpaper", true);
    settings.SetInt("index", wallpaper_index_);
    settings.SetString("city", wallpaper_city_);
    settings.SetBool("weather_valid", !wallpaper_condition_.empty());
    if (!wallpaper_condition_.empty()) {
        settings.SetString("condition", wallpaper_condition_);
        settings.SetInt("temperature_c", wallpaper_temperature_c_);
        settings.SetInt("high_c", wallpaper_high_c_);
        settings.SetInt("low_c", wallpaper_low_c_);
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

bool EmoteDisplay::InitializeTouchSurface()
{
    if (!touch_surface_image_data_.empty()) {
        return true;
    }
    const size_t pixel_count = static_cast<size_t>(kTouchSurfaceWidth) * kTouchSurfaceHeight;
    touch_surface_image_data_.assign(pixel_count * 3, 0);
    touch_surface_image_.header.magic = C_ARRAY_HEADER_MAGIC;
    touch_surface_image_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    touch_surface_image_.header.flags = 0;
    touch_surface_image_.header.w = kTouchSurfaceWidth;
    touch_surface_image_.header.h = kTouchSurfaceHeight;
    touch_surface_image_.header.stride = kTouchSurfaceWidth * sizeof(uint16_t);
    touch_surface_image_.header.reserved = 0;
    touch_surface_image_.data_size = touch_surface_image_data_.size();
    touch_surface_image_.data = touch_surface_image_data_.data();
    return true;
}

void EmoteDisplay::RenderTouchSurface(TouchView view, int selected_row)
{
    if (!InitializeTouchSurface()) {
        return;
    }

    std::fill(touch_surface_image_data_.begin(), touch_surface_image_data_.end(), 0);
    const bool transparent_background = view == TouchView::kWallpaper || view == TouchView::kPlayback;
    if (!transparent_background) {
        const size_t pixel_count = static_cast<size_t>(kTouchSurfaceWidth) * kTouchSurfaceHeight;
        auto* colors = reinterpret_cast<uint16_t*>(touch_surface_image_data_.data());
        auto* alphas = touch_surface_image_data_.data() + pixel_count * sizeof(uint16_t);
        for (int y = 0; y < kTouchSurfaceHeight; ++y) {
            uint32_t color = MixTouchUiColor(0x071019, 0x0A0F17, y * 255 / kTouchSurfaceHeight);
            if (y < 132) {
                color = MixTouchUiColor(color, 0x10303A, (132 - y) * 54 / 132);
            }
            std::fill_n(colors + static_cast<size_t>(y) * kTouchSurfaceWidth,
                        kTouchSurfaceWidth, TouchUiColor(color));
            std::memset(alphas + static_cast<size_t>(y) * kTouchSurfaceWidth,
                        255, kTouchSurfaceWidth);
        }
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               154, 19, 52, 4, 2, 0x6CE7F2, 210);
    }

    if (view == TouchView::kMenu) {
        static constexpr std::array<std::array<int, 2>, 4> positions = {{
            {43, 82}, {184, 82}, {43, 174}, {184, 174},
        }};
        for (size_t i = 0; i < positions.size(); ++i) {
            const int x = positions[i][0];
            const int y = positions[i][1];
            FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                   x, y, 133, 82, 18, 0x263443);
            FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                   x + 1, y + 1, 131, 80, 17, 0x111B27);
            FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                   x + 16, y + 13, 24, 4, 2, 0x58DCE8, i == 0 ? 255 : 145);
        }
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               114, 278, 132, 34, 17, 0x111A24);
    } else if (view == TouchView::kSlider) {
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               68, 101, 224, 70, 24, 0x273443);
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               69, 102, 222, 68, 23, 0x111B27);
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               90, 264, 180, 34, 17, 0x111A24);
    } else if (view == TouchView::kWallpaper) {
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               58, 28, 244, 46, 22, 0x071019, 185);
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               76, 276, 208, 50, 20, 0x071019, 180);
    } else if (view == TouchView::kMusic) {
        for (int row = 0; row < 5; ++row) {
            const int y = 88 + row * 36;
            const bool selected = row == selected_row;
            FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                   46, y, 268, 30, 12, selected ? 0x51DDE9 : 0x263443);
            FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                   47, y + 1, 266, 28, 11, selected ? 0x143743 : 0x111B27);
            if (selected) {
                FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                       52, y + 7, 4, 16, 2, 0x64EAF4);
            }
        }
        static constexpr std::array<int, 3> control_x = {66, 144, 222};
        static constexpr std::array<int, 3> control_width = {68, 72, 68};
        for (size_t i = 0; i < control_x.size(); ++i) {
            FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                   control_x[i], 282, control_width[i], 40, 18,
                                   i == 1 ? 0x56E0EA : 0x263443);
            FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                                   control_x[i] + 1, 283, control_width[i] - 2, 38, 17,
                                   i == 1 ? 0x116678 : 0x111B27);
        }
    } else if (view == TouchView::kPlayback) {
        FillTouchUiRoundedRect(touch_surface_image_data_, kTouchSurfaceWidth, kTouchSurfaceHeight,
                               58, 28, 150, 48, 22, 0x071019, 205);
    }
}

bool EmoteDisplay::InitializeTouchSliderVisual()
{
    if (!touch_slider_visual_image_data_.empty()) {
        return true;
    }
    const size_t pixel_count = static_cast<size_t>(kTouchSliderVisualWidth) * kTouchSliderVisualHeight;
    touch_slider_visual_image_data_.assign(pixel_count * 3, 0);
    touch_slider_visual_image_.header.magic = C_ARRAY_HEADER_MAGIC;
    touch_slider_visual_image_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    touch_slider_visual_image_.header.flags = 0;
    touch_slider_visual_image_.header.w = kTouchSliderVisualWidth;
    touch_slider_visual_image_.header.h = kTouchSliderVisualHeight;
    touch_slider_visual_image_.header.stride = kTouchSliderVisualWidth * sizeof(uint16_t);
    touch_slider_visual_image_.header.reserved = 0;
    touch_slider_visual_image_.data_size = touch_slider_visual_image_data_.size();
    touch_slider_visual_image_.data = touch_slider_visual_image_data_.data();
    return true;
}

void EmoteDisplay::RenderTouchSliderVisual(int value)
{
    if (!InitializeTouchSliderVisual()) {
        return;
    }
    value = std::clamp(value, 0, 100);
    std::fill(touch_slider_visual_image_data_.begin(), touch_slider_visual_image_data_.end(), 0);
    constexpr int track_x = 10;
    constexpr int track_width = 260;
    const int knob_x = track_x + value * track_width / 100;
    FillTouchUiRoundedRect(touch_slider_visual_image_data_, kTouchSliderVisualWidth,
                           kTouchSliderVisualHeight, track_x, 16, track_width, 8, 4, 0x2D3948);
    if (value > 0) {
        FillTouchUiRoundedRect(touch_slider_visual_image_data_, kTouchSliderVisualWidth,
                               kTouchSliderVisualHeight, track_x, 16,
                               std::max(8, knob_x - track_x), 8, 4, 0x58DCE8);
    }
    FillTouchUiCircle(touch_slider_visual_image_data_, kTouchSliderVisualWidth,
                      kTouchSliderVisualHeight, knob_x, 20, 13, 0x081019, 150);
    FillTouchUiCircle(touch_slider_visual_image_data_, kTouchSliderVisualWidth,
                      kTouchSliderVisualHeight, knob_x, 20, 10, 0xF2FCFD);
}

bool EmoteDisplay::EnsureTouchSettingsUi()
{
    if (!emote_handle_) {
        return false;
    }
    if (touch_background_ && touch_wallpaper_image_ && touch_surface_ &&
        touch_slider_visual_ && touch_title_ && touch_back_ && touch_close_ && touch_footer_) {
        return true;
    }

    touch_background_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL,
                                                  "touch_background");
    // The renderer paints objects in creation order. Keep a dedicated preview
    // above the settings background and below every control.
    touch_wallpaper_image_ = emote_create_obj_by_type(
        emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "touch_wallpaper_image");
    touch_surface_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "touch_surface");
    touch_slider_visual_ = emote_create_obj_by_type(
        emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "touch_slider_visual");
    touch_title_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "touch_title");
    touch_back_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "touch_back");
    touch_close_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "touch_close");
    for (size_t i = 0; i < touch_rows_.size(); ++i) {
        const std::string name = "touch_row_" + std::to_string(i);
        touch_rows_[i] = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, name.c_str());
    }
    for (size_t i = 0; i < touch_statuses_.size(); ++i) {
        const std::string name = "touch_status_" + std::to_string(i);
        touch_statuses_[i] = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, name.c_str());
    }
    touch_footer_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, "touch_footer");
    for (size_t i = 0; i < touch_controls_.size(); ++i) {
        const std::string name = "touch_control_" + std::to_string(i);
        touch_controls_[i] = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, name.c_str());
    }

    bool ready = touch_background_ && touch_wallpaper_image_ && touch_surface_ &&
                 touch_slider_visual_ && touch_title_ && touch_back_ && touch_close_ &&
                 touch_footer_ && InitializeTouchSurface() && InitializeTouchSliderVisual();
    for (auto* row : touch_rows_) ready = ready && row;
    for (auto* status : touch_statuses_) ready = ready && status;
    for (auto* control : touch_controls_) ready = ready && control;
    if (!ready) {
        ESP_LOGE(TAG, "Unable to create touch settings objects");
        return false;
    }

    const gfx_font_t touch_font = EnsureCommonTextFont();
    emote_lock(emote_handle_);
    gfx_obj_set_size(touch_background_, width_, height_);
    gfx_label_set_text(touch_background_, "");
    gfx_label_set_bg_enable(touch_background_, true);
    gfx_label_set_bg_color(touch_background_, GFX_COLOR_HEX(0x071019));
    gfx_label_set_opa(touch_background_, 255);
    gfx_obj_set_pos(touch_wallpaper_image_, 0, 0);
    gfx_obj_set_visible(touch_wallpaper_image_, false);
    gfx_img_set_src(touch_surface_, &touch_surface_image_);
    gfx_obj_set_pos(touch_surface_, 0, 0);
    gfx_obj_set_visible(touch_surface_, false);
    gfx_img_set_src(touch_slider_visual_, &touch_slider_visual_image_);
    gfx_obj_set_pos(touch_slider_visual_, 40, 182);
    gfx_obj_set_visible(touch_slider_visual_, false);

    auto style_text = [touch_font](gfx_obj_t* obj, gfx_color_t color) {
        gfx_label_set_font(obj, touch_font);
        gfx_label_set_color(obj, color);
        gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(obj, GFX_LABEL_LONG_CLIP);
    };
    style_text(touch_title_, GFX_COLOR_HEX(0xF5F8FA));
    style_text(touch_back_, GFX_COLOR_HEX(0x64EAF4));
    style_text(touch_close_, GFX_COLOR_HEX(0x9CAABA));
    style_text(touch_footer_, GFX_COLOR_HEX(0x8392A3));
    for (auto* row : touch_rows_) {
        style_text(row, GFX_COLOR_HEX(0xF5F8FA));
        gfx_label_set_bg_enable(row, false);
    }
    for (auto* status : touch_statuses_) {
        style_text(status, GFX_COLOR_HEX(0x91A1B3));
        gfx_label_set_bg_enable(status, false);
    }
    gfx_label_set_bg_enable(touch_back_, false);
    gfx_label_set_bg_enable(touch_close_, false);
    for (auto* control : touch_controls_) {
        style_text(control, GFX_COLOR_HEX(0xF5F8FA));
        gfx_label_set_bg_enable(control, false);
    }
    emote_unlock(emote_handle_);
    HideTouchObjects();
    return true;
}

void EmoteDisplay::HideTouchObjects()
{
    if (!emote_handle_ || !touch_background_) {
        return;
    }
    emote_lock(emote_handle_);
    for (auto* obj : {touch_background_, touch_wallpaper_image_, touch_surface_,
                      touch_slider_visual_, touch_title_, touch_back_, touch_close_, touch_footer_}) {
        if (obj && gfx_obj_get_visible(obj)) gfx_obj_set_visible(obj, false);
    }
    for (auto* row : touch_rows_)
        if (gfx_obj_get_visible(row)) gfx_obj_set_visible(row, false);
    for (auto* status : touch_statuses_)
        if (gfx_obj_get_visible(status)) gfx_obj_set_visible(status, false);
    for (auto* control : touch_controls_)
        if (gfx_obj_get_visible(control)) gfx_obj_set_visible(control, false);
    if (touch_music_return_ && gfx_obj_get_visible(touch_music_return_))
        gfx_obj_set_visible(touch_music_return_, false);
    emote_unlock(emote_handle_);
    touch_view_ = TouchView::kNone;
    touch_menu_offset_ = 1;
    touch_slider_value_ = -1;
    touch_row_visible_.fill(false);
}

void EmoteDisplay::RestoreTouchWallpaperPreview()
{
    if (!touch_wallpaper_preview_) {
        return;
    }
    const bool loaded = LoadWallpaper(wallpaper_index_);
    SetWallpaperNativeUiVisible(!touch_wallpaper_restore_visible_);
    emote_lock(emote_handle_);
    if (loaded && touch_wallpaper_restore_visible_) {
        gfx_img_set_src(wallpaper_image_, &wallpaper_image_dsc_);
        gfx_obj_set_visible(wallpaper_image_, true);
    } else {
        gfx_obj_set_visible(wallpaper_image_, false);
    }
    emote_unlock(emote_handle_);
    wallpaper_visible_ = touch_wallpaper_restore_visible_ && loaded;
    touch_wallpaper_preview_ = false;
}

void EmoteDisplay::UpdateTouchMenuOffset(int reveal_height)
{
    const int offset = std::clamp(reveal_height, 0, height_) - height_;
    if (offset == touch_menu_offset_) return;
    emote_lock(emote_handle_);
    gfx_obj_set_pos(touch_background_, 0, offset);
    gfx_obj_set_pos(touch_surface_, 0, offset);
    gfx_obj_set_pos(touch_title_, 110, 34 + offset);
    gfx_obj_set_pos(touch_close_, 255, 36 + offset);
    static constexpr std::array<std::array<int, 2>, 4> label_positions = {{
        {52, 102}, {193, 102}, {52, 194}, {193, 194},
    }};
    for (size_t i = 0; i < 4; ++i) {
        gfx_obj_set_pos(touch_rows_[i], label_positions[i][0], label_positions[i][1] + offset);
        gfx_obj_set_pos(touch_statuses_[i], label_positions[i][0], label_positions[i][1] + 29 + offset);
    }
    gfx_obj_set_pos(touch_footer_, 105, 281 + offset);
    emote_unlock(emote_handle_);
    touch_menu_offset_ = offset;
}

void EmoteDisplay::ShowTouchMenu(int reveal_height, const char* volume,
                                 const char* brightness, const char* wallpaper,
                                 const char* music)
{
    if (!EnsureTouchSettingsUi()) return;
    touch_music_overlay_suppressed_ = true;
    SetMusicOverlayVisible(false);
    RestoreTouchWallpaperPreview();
    const bool initialize = touch_view_ != TouchView::kMenu;
    if (initialize) {
        HideTouchObjects();
        touch_view_ = TouchView::kMenu;
        touch_settings_visible_ = true;
        static constexpr std::array<const char*, 4> defaults = {
            "音量", "亮度", "壁纸", "音乐",
        };
        const std::array<const char*, 4> states = {volume, brightness, wallpaper, music};
        RenderTouchSurface(TouchView::kMenu);
        emote_lock(emote_handle_);
        gfx_label_set_opa(touch_background_, 255);
        gfx_obj_set_visible(touch_background_, true);
        gfx_img_set_src(touch_surface_, &touch_surface_image_);
        gfx_obj_set_visible(touch_surface_, true);
        gfx_obj_set_size(touch_title_, 140, 32);
        gfx_label_set_font(touch_title_, EnsureCommonTextFont());
        gfx_label_set_text(touch_title_, "控制中心");
        gfx_obj_set_visible(touch_title_, true);
        gfx_obj_set_size(touch_close_, 62, 32);
        gfx_label_set_text(touch_close_, "关闭");
        gfx_obj_set_visible(touch_close_, true);
        for (size_t i = 0; i < 4; ++i) {
            gfx_label_set_font(touch_rows_[i], EnsureCommonTextFont());
            gfx_label_set_color(touch_rows_[i], GFX_COLOR_HEX(0xF5F8FA));
            gfx_label_set_bg_enable(touch_rows_[i], false);
            gfx_obj_set_size(touch_rows_[i], 115, 27);
            gfx_label_set_text(touch_rows_[i], defaults[i]);
            gfx_obj_set_visible(touch_rows_[i], true);
            gfx_obj_set_size(touch_statuses_[i], 115, 27);
            const std::string state = states[i] && states[i][0]
                                          ? TruncateTouchText(states[i], 105)
                                          : "--";
            gfx_label_set_text(touch_statuses_[i], state.c_str());
            gfx_obj_set_visible(touch_statuses_[i], true);
        }
        gfx_obj_set_size(touch_footer_, 150, 27);
        gfx_label_set_text(touch_footer_, "上滑收起");
        gfx_obj_set_visible(touch_footer_, true);
        emote_unlock(emote_handle_);
        touch_menu_offset_ = 1;
    }
    UpdateTouchMenuOffset(reveal_height);
}

void EmoteDisplay::ShowTouchSlider(const char* title, int value)
{
    if (!EnsureTouchSettingsUi()) return;
    touch_music_overlay_suppressed_ = true;
    SetMusicOverlayVisible(false);
    RestoreTouchWallpaperPreview();
    const std::string requested_title = title ? title : "设置";
    if (touch_view_ == TouchView::kSlider && touch_slider_title_ == requested_title) {
        UpdateTouchSliderValue(value);
        return;
    }
    HideTouchObjects();
    touch_view_ = TouchView::kSlider;
    touch_slider_title_ = requested_title;
    touch_settings_visible_ = true;
    RenderTouchSurface(TouchView::kSlider);
    RenderTouchSliderVisual(value);
    emote_lock(emote_handle_);
    gfx_label_set_opa(touch_background_, 255);
    gfx_obj_set_pos(touch_background_, 0, 0);
    gfx_obj_set_visible(touch_background_, true);
    gfx_img_set_src(touch_surface_, &touch_surface_image_);
    gfx_obj_set_pos(touch_surface_, 0, 0);
    gfx_obj_set_visible(touch_surface_, true);
    gfx_img_set_src(touch_slider_visual_, &touch_slider_visual_image_);
    gfx_obj_set_size(touch_slider_visual_, kTouchSliderVisualWidth, kTouchSliderVisualHeight);
    gfx_obj_set_pos(touch_slider_visual_, 40, 182);
    gfx_obj_set_visible(touch_slider_visual_, true);
    gfx_obj_set_size(touch_back_, 80, 36);
    gfx_obj_set_pos(touch_back_, 68, 40);
    gfx_label_set_text(touch_back_, "< 返回");
    gfx_obj_set_visible(touch_back_, true);
    gfx_obj_set_size(touch_title_, 112, 36);
    gfx_obj_set_pos(touch_title_, 140, 40);
    gfx_label_set_font(touch_title_, EnsureCommonTextFont());
    gfx_label_set_text(touch_title_, title ? title : "设置");
    gfx_obj_set_visible(touch_title_, true);
    gfx_label_set_bg_enable(touch_rows_[0], false);
    gfx_label_set_font(touch_rows_[0], static_cast<gfx_font_t>(
        const_cast<lv_font_t*>(&font_puhui_basic_30_4)));
    gfx_label_set_color(touch_rows_[0], GFX_COLOR_HEX(0xF5FBFC));
    gfx_obj_set_size(touch_rows_[0], 180, 48);
    gfx_obj_set_pos(touch_rows_[0], 90, 112);
    gfx_label_set_text(touch_rows_[0], "");
    gfx_obj_set_visible(touch_rows_[0], true);
    gfx_obj_set_size(touch_footer_, 180, 28);
    gfx_obj_set_pos(touch_footer_, 90, 267);
    gfx_label_set_text(touch_footer_, "松手后自动保存");
    gfx_obj_set_visible(touch_footer_, true);
    emote_unlock(emote_handle_);
    touch_slider_value_ = -1;
    UpdateTouchSliderValue(value);
}

void EmoteDisplay::UpdateTouchSliderValue(int value)
{
    if (!emote_handle_ || touch_view_ != TouchView::kSlider) return;
    value = std::clamp(value, 0, 100);
    if (value == touch_slider_value_) return;
    RenderTouchSliderVisual(value);
    emote_lock(emote_handle_);
    gfx_label_set_text_fmt(touch_rows_[0], "%d%%", value);
    gfx_img_set_src(touch_slider_visual_, &touch_slider_visual_image_);
    emote_unlock(emote_handle_);
    touch_slider_value_ = value;
}

int EmoteDisplay::GetTouchWallpaperCount()
{
    return static_cast<int>(WallpaperCount());
}

std::string EmoteDisplay::GetTouchWallpaperName(int index)
{
    static constexpr std::array<const char*, 3> internal_names = {"晨曦", "日间", "夜色"};
    if (index >= 0 && index < static_cast<int>(internal_names.size())) {
        return internal_names[index];
    }
    const int custom_index = index - static_cast<int>(kWallpaperAssets.size());
    std::lock_guard<std::mutex> lock(wallpaper_data_mutex_);
    if (custom_index < 0 || custom_index >= static_cast<int>(custom_wallpaper_paths_.size())) {
        return "壁纸";
    }
    const std::string& path = custom_wallpaper_paths_[custom_index];
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool EmoteDisplay::ShowTouchWallpaper(int index)
{
    if (!EnsureTouchSettingsUi()) return false;
    touch_music_overlay_suppressed_ = true;
    SetMusicOverlayVisible(false);
    if (!touch_wallpaper_preview_) {
        touch_wallpaper_restore_visible_ = wallpaper_visible_;
    }
    if (!LoadWallpaper(index)) return false;
    HideTouchObjects();
    touch_view_ = TouchView::kWallpaper;
    touch_settings_visible_ = true;
    touch_wallpaper_preview_ = true;
    SetWallpaperNativeUiVisible(false);
    const std::string name = GetTouchWallpaperName(index);
    const int count = GetTouchWallpaperCount();
    RenderTouchSurface(TouchView::kWallpaper);
    emote_lock(emote_handle_);
    gfx_obj_set_visible(wallpaper_image_, false);
    gfx_obj_set_visible(wallpaper_fade_, false);
    gfx_label_set_opa(touch_background_, 255);
    gfx_obj_set_pos(touch_background_, 0, 0);
    gfx_obj_set_visible(touch_background_, true);
    gfx_img_set_src(touch_wallpaper_image_, &wallpaper_image_dsc_);
    gfx_obj_set_pos(touch_wallpaper_image_, 0, 0);
    gfx_obj_set_visible(touch_wallpaper_image_, true);
    gfx_img_set_src(touch_surface_, &touch_surface_image_);
    gfx_obj_set_pos(touch_surface_, 0, 0);
    gfx_obj_set_visible(touch_surface_, true);
    gfx_label_set_bg_enable(touch_back_, false);
    gfx_obj_set_size(touch_back_, 78, 32);
    gfx_obj_set_pos(touch_back_, 75, 35);
    gfx_label_set_text(touch_back_, "< 返回");
    gfx_obj_set_visible(touch_back_, true);
    gfx_obj_set_size(touch_title_, 80, 32);
    gfx_obj_set_pos(touch_title_, 140, 35);
    gfx_label_set_font(touch_title_, EnsureCommonTextFont());
    gfx_label_set_text(touch_title_, "壁纸");
    gfx_obj_set_visible(touch_title_, true);
    gfx_label_set_bg_enable(touch_rows_[0], false);
    gfx_label_set_font(touch_rows_[0], EnsureCommonTextFont());
    gfx_label_set_color(touch_rows_[0], GFX_COLOR_HEX(0xF5F8FA));
    gfx_obj_set_size(touch_rows_[0], 180, 25);
    gfx_obj_set_pos(touch_rows_[0], 90, 278);
    const std::string display_name = TruncateTouchText(name, 170);
    gfx_label_set_text(touch_rows_[0], display_name.c_str());
    gfx_obj_set_visible(touch_rows_[0], true);
    gfx_obj_set_size(touch_footer_, 220, 23);
    gfx_obj_set_pos(touch_footer_, 70, 301);
    gfx_label_set_text_fmt(touch_footer_, "%d/%d · 滑动 · 点按应用", index + 1, count);
    gfx_obj_set_visible(touch_footer_, true);
    emote_unlock(emote_handle_);
    return true;
}

bool EmoteDisplay::ApplyTouchWallpaper(int index)
{
    if (index < 0 || index >= GetTouchWallpaperCount()) return false;
    wallpaper_index_ = index;
    SaveWallpaperSettings();
    wallpaper_shown_since_us_ = esp_timer_get_time();
    return true;
}

void EmoteDisplay::ShowTouchMusic(const std::vector<std::string>& titles, int first_index,
                                  int selected_index, const char* status,
                                  const char* primary_control)
{
    if (!EnsureTouchSettingsUi()) return;
    touch_music_overlay_suppressed_ = true;
    SetMusicOverlayVisible(false);
    RestoreTouchWallpaperPreview();
    const bool initialize = touch_view_ != TouchView::kMusic;
    if (initialize) {
        HideTouchObjects();
        touch_view_ = TouchView::kMusic;
    }
    touch_settings_visible_ = true;
    RenderTouchSurface(TouchView::kMusic, selected_index - first_index);
    emote_lock(emote_handle_);
    gfx_img_set_src(touch_surface_, &touch_surface_image_);
    gfx_obj_set_pos(touch_surface_, 0, 0);
    gfx_obj_set_visible(touch_surface_, true);
    if (initialize) {
        gfx_label_set_opa(touch_background_, 255);
        gfx_obj_set_pos(touch_background_, 0, 0);
        gfx_obj_set_visible(touch_background_, true);
        gfx_obj_set_size(touch_back_, 80, 36);
        gfx_obj_set_pos(touch_back_, 68, 37);
        gfx_label_set_text(touch_back_, "< 返回");
        gfx_obj_set_visible(touch_back_, true);
        gfx_obj_set_size(touch_title_, 100, 36);
        gfx_obj_set_pos(touch_title_, 140, 37);
        gfx_label_set_font(touch_title_, EnsureCommonTextFont());
        gfx_label_set_text(touch_title_, "音乐");
        gfx_obj_set_visible(touch_title_, true);
    }
    for (size_t row = 0; row < touch_rows_.size(); ++row) {
        auto* row_label = touch_rows_[row];
        const int index = first_index + static_cast<int>(row);
        if (initialize) {
            gfx_obj_set_size(row_label, 242, 30);
            gfx_obj_set_pos(row_label, 59, 88 + static_cast<int>(row) * 36);
        }
        if (index >= 0 && index < static_cast<int>(titles.size())) {
            const std::string label = TruncateTouchText(titles[index], 230);
            gfx_label_set_text(row_label, label.c_str());
            gfx_obj_set_visible(row_label, true);
            touch_row_visible_[row] = true;
            gfx_label_set_color(row_label, GFX_COLOR_HEX(
                index == selected_index ? 0xF5FCFD : 0xC4CEDA));
        } else {
            if (touch_row_visible_[row]) gfx_obj_set_visible(row_label, false);
            touch_row_visible_[row] = false;
        }
    }
    if (titles.empty() && status && status[0]) {
        // The library scan completes asynchronously and the next update is incremental,
        // so row 0 must keep the same geometry used by populated lists.
        gfx_label_set_text(touch_rows_[0], status);
        gfx_obj_set_visible(touch_rows_[0], true);
    }
    if (initialize) {
        const std::array<const char*, 3> controls = {
            "上一首", primary_control && primary_control[0] ? primary_control : "播放", "下一首"};
        for (size_t i = 0; i < touch_controls_.size(); ++i) {
            gfx_obj_set_size(touch_controls_[i], i == 1 ? 72 : 66, 40);
            gfx_obj_set_pos(touch_controls_[i], 68 + static_cast<int>(i) * 78, 282);
            gfx_label_set_bg_enable(touch_controls_[i], false);
            gfx_label_set_text(touch_controls_[i], controls[i]);
            gfx_obj_set_visible(touch_controls_[i], true);
        }
        touch_primary_control_ = controls[1];
    } else {
        const std::string control = primary_control && primary_control[0] ? primary_control : "播放";
        if (control != touch_primary_control_) {
            gfx_label_set_text(touch_controls_[1], control.c_str());
            touch_primary_control_ = control;
        }
    }
    emote_unlock(emote_handle_);
}

void EmoteDisplay::ShowTouchMusicPlayback()
{
    if (!EnsureMusicUi() || !EnsureTouchSettingsUi()) return;
    HideTouchObjects();
    touch_view_ = TouchView::kPlayback;
    touch_settings_visible_ = true;
    touch_music_overlay_suppressed_ = false;
    SetMusicOverlayVisible(true);
    RenderTouchSurface(TouchView::kPlayback);

    if (!touch_music_return_) {
        touch_music_return_ = emote_create_obj_by_type(
            emote_handle_, EMOTE_OBJ_TYPE_LABEL, "touch_music_return");
        if (!touch_music_return_) {
            ESP_LOGE(TAG, "Unable to create music playback return control");
            return;
        }
        emote_lock(emote_handle_);
        gfx_label_set_font(touch_music_return_, EnsureCommonTextFont());
        gfx_label_set_color(touch_music_return_, GFX_COLOR_HEX(0x62D9FF));
        gfx_label_set_text_align(touch_music_return_, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(touch_music_return_, GFX_LABEL_LONG_CLIP);
        gfx_label_set_bg_enable(touch_music_return_, false);
        emote_unlock(emote_handle_);
    }

    emote_lock(emote_handle_);
    gfx_img_set_src(touch_surface_, &touch_surface_image_);
    gfx_obj_set_pos(touch_surface_, 0, 0);
    gfx_obj_set_visible(touch_surface_, true);
    gfx_obj_set_size(touch_music_return_, 126, 38);
    gfx_obj_set_pos(touch_music_return_, 69, 33);
    gfx_label_set_text(touch_music_return_, "< 音乐列表");
    gfx_obj_set_visible(touch_music_return_, true);
    emote_unlock(emote_handle_);
}

void EmoteDisplay::HideTouchSettings()
{
    if (!touch_settings_visible_) return;
    RestoreTouchWallpaperPreview();
    HideTouchObjects();
    touch_settings_visible_ = false;
    touch_music_overlay_suppressed_ = false;
    SetMusicOverlayVisible(true);
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
        HideWallpaper();
        wallpaper_suppressed_until_us_ = esp_timer_get_time() +
            static_cast<int64_t>(duration_ms) * 1000 + kWallpaperIdleDelayUs;
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
