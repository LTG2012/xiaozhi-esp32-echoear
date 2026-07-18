#include "sd_music_player.h"

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "config.h"
#include "display.h"
#include "mcp_server.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cinttypes>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sstream>
#include <sys/stat.h>

#include <driver/sdmmc_host.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_simple_dec.h>
#include <esp_log.h>
#include <esp_mp3_dec.h>
#include <esp_random.h>
#include <esp_vfs_fat.h>
#include <ff.h>

namespace {

constexpr char kTag[] = "SdMusicPlayer";
constexpr char kMountPoint[] = "/sdcard";
constexpr size_t kMaxLrcFileSize = 512 * 1024;
constexpr size_t kLyricsCharsPerVisualRow = 15;
constexpr int kLyricsMaxVisualRows = 7;

bool PathExists(const std::string& path) {
    struct stat info = {};
    return stat(path.c_str(), &info) == 0;
}

size_t Utf8CodepointCount(const std::string& text) {
    size_t count = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

int EstimateLyricsRows(const std::string& text, bool current) {
    const size_t characters = Utf8CodepointCount(text) + (current ? 2 : 0);
    return std::max<int>(1, (characters + kLyricsCharsPerVisualRow - 1) /
                                kLyricsCharsPerVisualRow);
}

void AppendUtf8(std::string& output, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

bool IsValidUtf8(const std::string& input) {
    for (size_t i = 0; i < input.size();) {
        const auto first = static_cast<uint8_t>(input[i]);
        if (first <= 0x7F) {
            ++i;
            continue;
        }

        size_t continuation_count = 0;
        uint32_t codepoint = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            continuation_count = 1;
            codepoint = first & 0x1F;
        } else if (first >= 0xE0 && first <= 0xEF) {
            continuation_count = 2;
            codepoint = first & 0x0F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuation_count = 3;
            codepoint = first & 0x07;
        } else {
            return false;
        }
        if (i + continuation_count >= input.size()) {
            return false;
        }
        for (size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto byte = static_cast<uint8_t>(input[i + offset]);
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3F);
        }
        if ((continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
            return false;
        }
        i += continuation_count + 1;
    }
    return true;
}

std::string DecodeUtf16(const std::string& input, bool big_endian) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 2; i + 1 < input.size();) {
        const auto first = static_cast<uint8_t>(input[i]);
        const auto second = static_cast<uint8_t>(input[i + 1]);
        uint32_t codepoint = big_endian ? (first << 8) | second : (second << 8) | first;
        i += 2;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 1 < input.size()) {
            const auto next_first = static_cast<uint8_t>(input[i]);
            const auto next_second = static_cast<uint8_t>(input[i + 1]);
            const uint32_t low = big_endian ? (next_first << 8) | next_second
                                            : (next_second << 8) | next_first;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                codepoint = 0xFFFD;
            }
        } else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
        }
        AppendUtf8(output, codepoint);
    }
    return output;
}

std::string DecodeGbk(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2);
    for (size_t i = 0; i < input.size();) {
        const auto first = static_cast<uint8_t>(input[i]);
        if (first < 0x80) {
            output.push_back(static_cast<char>(first));
            ++i;
            continue;
        }
        if (i + 1 < input.size()) {
            const auto second = static_cast<uint8_t>(input[i + 1]);
            if (first >= 0x81 && first <= 0xFE && second >= 0x40 && second <= 0xFE && second != 0x7F) {
                const WCHAR unicode = ff_oem2uni(static_cast<WCHAR>((first << 8) | second), 936);
                AppendUtf8(output, unicode == 0 ? 0xFFFD : unicode);
                i += 2;
                continue;
            }
        }
        AppendUtf8(output, 0xFFFD);
        ++i;
    }
    return output;
}

std::string DecodeLrcContent(std::string input) {
    if (input.size() >= 3 && static_cast<uint8_t>(input[0]) == 0xEF &&
        static_cast<uint8_t>(input[1]) == 0xBB && static_cast<uint8_t>(input[2]) == 0xBF) {
        ESP_LOGI(kTag, "LRC encoding: UTF-8 BOM");
        input.erase(0, 3);
        return input;
    }
    if (input.size() >= 2 && static_cast<uint8_t>(input[0]) == 0xFF &&
        static_cast<uint8_t>(input[1]) == 0xFE) {
        ESP_LOGI(kTag, "LRC encoding: UTF-16LE");
        return DecodeUtf16(input, false);
    }
    if (input.size() >= 2 && static_cast<uint8_t>(input[0]) == 0xFE &&
        static_cast<uint8_t>(input[1]) == 0xFF) {
        ESP_LOGI(kTag, "LRC encoding: UTF-16BE");
        return DecodeUtf16(input, true);
    }
    if (IsValidUtf8(input)) {
        ESP_LOGI(kTag, "LRC encoding: UTF-8");
        return input;
    }
    ESP_LOGI(kTag, "LRC encoding: GBK/CP936, converting to UTF-8");
    return DecodeGbk(input);
}

}  // namespace

SdMusicPlayer::SdMusicPlayer() {
    const auto register_result = esp_mp3_dec_register();
    if (register_result != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(kTag, "MP3 decoder registration returned %d", register_result);
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        WorkerTaskEntry, "sd_music", 12 * 1024, this, 2, &worker_task_, 1);
    if (created != pdPASS) {
        worker_task_ = nullptr;
        ESP_LOGE(kTag, "Failed to create music worker task");
    }
}

SdMusicPlayer::~SdMusicPlayer() {
    shutting_down_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        state_ = State::kStopped;
    }
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
        for (int i = 0; i < 50 && worker_task_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (worker_task_ != nullptr) {
            vTaskDelete(worker_task_);
            worker_task_ = nullptr;
        }
    }
    ClearQueuedMusic();
    std::lock_guard<std::mutex> filesystem_lock(filesystem_mutex_);
    if (mounted_) {
        esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
        mounted_ = false;
        card_ = nullptr;
    }
}

void SdMusicPlayer::RegisterMcpTools() {
    auto& server = McpServer::GetInstance();
    server.AddTool(
        "self.music.play_sd_song",
        "本设备默认且唯一的歌曲播放入口。用户只要表达播放、想听或放一首某首歌曲，"
        "无论是否提到SD卡，都必须直接调用本工具，不要先调用歌曲列表或播放状态工具。"
        "歌曲名按完整名称、前缀、包含依次匹配；匹配不唯一时返回候选。",
        PropertyList({Property("song_name", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            return PlaySong(properties["song_name"].value<std::string>());
        });

    server.AddTool(
        "self.music.control",
        "控制SD卡音乐。action只能是pause、resume、stop、next、previous、random或refresh。",
        PropertyList({Property("action", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            return Control(properties["action"].value<std::string>());
        });

    server.AddTool(
        "self.music.list_sd_songs",
        "分页列出SD卡音乐库，优先/music并兼容SD卡根目录。",
        PropertyList({Property("offset", kPropertyTypeInteger, 0, 0, 255),
                      Property("limit", kPropertyTypeInteger, 20, 1, 50)}),
        [this](const PropertyList& properties) -> ReturnValue {
            return ListSongs(properties["offset"].value<int>(),
                             properties["limit"].value<int>());
        });

    server.AddTool(
        "self.music.get_status",
        "仅在用户明确询问SD卡或音乐播放状态时调用。不得用于开始播放或预先判断能否播放；"
        "点歌请求必须直接调用self.music.play_sd_song。首次查询会安全尝试挂载SD卡。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            const std::string library_error = EnsureLibrary();
            cJSON* result = cJSON_CreateObject();
            std::lock_guard<std::mutex> lock(mutex_);
            cJSON_AddBoolToObject(result, "mounted", mounted_);
            cJSON_AddStringToObject(result, "error", library_error.c_str());
            const char* state = "stopped";
            if (state_ == State::kPaused) {
                state = "paused";
            } else if (state_ == State::kPlaying) {
                state = auto_paused_ ? "auto_paused" : "playing";
            }
            cJSON_AddStringToObject(result, "state", state);
            if (current_index_ >= 0 && current_index_ < static_cast<int>(songs_.size())) {
                cJSON_AddStringToObject(result, "song", songs_[current_index_].title.c_str());
            } else {
                cJSON_AddStringToObject(result, "song", "");
            }
            cJSON_AddNumberToObject(result, "progress_ms",
                                    submitted_samples_ * 1000ULL / kOutputSampleRate);
            cJSON_AddNumberToObject(result, "duration_ms",
                                    total_samples_ * 1000ULL / kOutputSampleRate);
            cJSON_AddBoolToObject(result, "lyrics", lyrics_available_);
            cJSON_AddNumberToObject(result, "song_count", songs_.size());
            return result;
        });
}

void SdMusicPlayer::WorkerTaskEntry(void* arg) {
    static_cast<SdMusicPlayer*>(arg)->WorkerTask();
}

void SdMusicPlayer::WorkerTask() {
    while (!shutting_down_) {
        Song song;
        uint32_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::kPlaying && current_index_ >= 0 &&
                current_index_ < static_cast<int>(songs_.size())) {
                song = songs_[current_index_];
                generation = generation_;
            }
        }

        if (generation == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            continue;
        }

        const DecodeResult result = DecodeSong(song, generation);
        if (result == DecodeResult::kFinished) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == generation_ && state_ == State::kPlaying && !songs_.empty()) {
                SelectTrackLocked((current_index_ + 1) % songs_.size());
            }
        } else if (result == DecodeResult::kError) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation == generation_) {
                    state_ = State::kStopped;
                    auto_paused_ = false;
                }
            }
            ClearQueuedMusic();
            ClearMusicText();
        }
    }

    worker_task_ = nullptr;
    vTaskDelete(nullptr);
}

SdMusicPlayer::DecodeResult SdMusicPlayer::DecodeSong(const Song& song, uint32_t generation) {
    auto lyrics = LoadLyrics(song.lyric_path);
    std::string initial_lyrics;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_lyrics_ = std::move(lyrics);
        lyrics_available_ = !current_lyrics_.empty();
        displayed_lyric_index_ = -1;
        initial_lyrics = BuildLyricsWindowLocked(-1);
    }
    ShowMusicText(song.title, initial_lyrics);

    FILE* file = fopen(song.path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGE(kTag, "Cannot open %s: %s", song.path.c_str(), strerror(errno));
        MarkCardUnavailable();
        return DecodeResult::kError;
    }

    uint64_t mp3_audio_bytes = 0;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long file_size = ftell(file);
        if (file_size > 0) {
            mp3_audio_bytes = static_cast<uint64_t>(file_size);
        }
        rewind(file);
    }
    uint8_t id3_header[10] = {};
    if (fread(id3_header, 1, sizeof(id3_header), file) == sizeof(id3_header) &&
        std::memcmp(id3_header, "ID3", 3) == 0) {
        const uint64_t id3_size = 10ULL +
            (static_cast<uint64_t>(id3_header[6] & 0x7F) << 21) +
            (static_cast<uint64_t>(id3_header[7] & 0x7F) << 14) +
            (static_cast<uint64_t>(id3_header[8] & 0x7F) << 7) +
            static_cast<uint64_t>(id3_header[9] & 0x7F);
        if (id3_size < mp3_audio_bytes) {
            mp3_audio_bytes -= id3_size;
        }
    }
    rewind(file);

    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const auto open_result = esp_audio_simple_dec_open(&decoder_config, &decoder);
    if (open_result != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(kTag, "Cannot open MP3 decoder: %d", open_result);
        fclose(file);
        return DecodeResult::kError;
    }

    std::vector<uint8_t> input(2048);
    std::vector<uint8_t> decoded(8192);
    std::vector<int16_t> pending_pcm;
    size_t pending_offset = 0;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    uint32_t resampler_source_rate = 0;
    DecodeResult result = DecodeResult::kFinished;
    bool card_io_error = false;

    while (!shutting_down_ && IsCurrentGeneration(generation)) {
        if (!WaitUntilPlayable(generation)) {
            result = DecodeResult::kChanged;
            break;
        }

        const size_t read_size = fread(input.data(), 1, input.size(), file);
        if (read_size == 0) {
            if (ferror(file)) {
                ESP_LOGE(kTag, "SD read failed for %s", song.path.c_str());
                result = DecodeResult::kError;
                card_io_error = true;
            }
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input.data(),
            .len = static_cast<uint32_t>(read_size),
            .eos = read_size < input.size() && feof(file),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        while (raw.len > 0 && IsCurrentGeneration(generation)) {
            esp_audio_simple_dec_out_t output = {
                .buffer = decoded.data(),
                .len = static_cast<uint32_t>(decoded.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            raw.consumed = 0;
            auto decode_result = esp_audio_simple_dec_process(decoder, &raw, &output);
            if (decode_result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && output.needed_size > decoded.size()) {
                decoded.resize(output.needed_size);
                continue;
            }
            if (decode_result != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(kTag, "MP3 decode failed: %d", decode_result);
                result = DecodeResult::kError;
                break;
            }

            if (output.decoded_size > 0) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                    info.bits_per_sample != 16 || info.channel == 0) {
                    ESP_LOGE(kTag, "Unsupported MP3 PCM format");
                    result = DecodeResult::kError;
                    break;
                }

                if (info.bitrate > 0 && mp3_audio_bytes > 0) {
                    // Some MP3 decoder backends report kbps (for example 128),
                    // while the public simple-decoder API documents bit/s.
                    const uint64_t bitrate_bps = info.bitrate < 1000
                        ? static_cast<uint64_t>(info.bitrate) * 1000ULL
                        : static_cast<uint64_t>(info.bitrate);
                    bool duration_ready = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (generation == generation_ && total_samples_ == 0) {
                            total_samples_ = mp3_audio_bytes * 8ULL * kOutputSampleRate /
                                             bitrate_bps;
                            duration_ready = total_samples_ > 0;
                        }
                    }
                    if (duration_ready) {
                        UpdateProgress(generation, true);
                        UpdatePlaybackVisualization(generation, -1, true);
                    }
                }

                const auto* interleaved = reinterpret_cast<const int16_t*>(output.buffer);
                const size_t total_samples = output.decoded_size / sizeof(int16_t);
                const size_t frame_count = total_samples / info.channel;
                std::vector<int16_t> mono(frame_count);
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    int32_t sum = 0;
                    for (uint8_t channel = 0; channel < info.channel; ++channel) {
                        sum += interleaved[frame * info.channel + channel];
                    }
                    mono[frame] = static_cast<int16_t>(sum / info.channel);
                }

                std::vector<int16_t> converted;
                if (info.sample_rate == kOutputSampleRate) {
                    converted = std::move(mono);
                } else {
                    if (resampler == nullptr || resampler_source_rate != info.sample_rate) {
                        if (resampler != nullptr) {
                            esp_ae_rate_cvt_close(resampler);
                            resampler = nullptr;
                        }
                        esp_ae_rate_cvt_cfg_t config = {
                            .src_rate = info.sample_rate,
                            .dest_rate = kOutputSampleRate,
                            .channel = 1,
                            .bits_per_sample = ESP_AUDIO_BIT16,
                            .complexity = 2,
                            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY,
                        };
                        if (esp_ae_rate_cvt_open(&config, &resampler) != ESP_AE_ERR_OK ||
                            resampler == nullptr) {
                            ESP_LOGE(kTag, "Cannot create %lu Hz resampler",
                                     static_cast<unsigned long>(info.sample_rate));
                            result = DecodeResult::kError;
                            break;
                        }
                        resampler_source_rate = info.sample_rate;
                    }
                    uint32_t capacity = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &capacity);
                    converted.resize(capacity);
                    uint32_t produced = capacity;
                    if (esp_ae_rate_cvt_process(resampler, mono.data(), mono.size(),
                                                converted.data(), &produced) != ESP_AE_ERR_OK) {
                        ESP_LOGE(kTag, "MP3 resampling failed");
                        result = DecodeResult::kError;
                        break;
                    }
                    converted.resize(produced);
                }

                pending_pcm.insert(pending_pcm.end(), converted.begin(), converted.end());
                while (pending_pcm.size() - pending_offset >= kPcmChunkSamples) {
                    std::vector<int16_t> chunk(
                        pending_pcm.begin() + pending_offset,
                        pending_pcm.begin() + pending_offset + kPcmChunkSamples);
                    if (!SubmitPcmChunk(chunk, generation)) {
                        result = DecodeResult::kChanged;
                        break;
                    }
                    pending_offset += kPcmChunkSamples;
                }
                if (pending_offset > 4096) {
                    pending_pcm.erase(pending_pcm.begin(), pending_pcm.begin() + pending_offset);
                    pending_offset = 0;
                }
            }

            if (result != DecodeResult::kFinished) {
                break;
            }
            if (raw.consumed == 0) {
                ESP_LOGE(kTag, "MP3 decoder made no progress");
                result = DecodeResult::kError;
                break;
            }
            const uint32_t consumed = std::min(raw.len, raw.consumed);
            raw.buffer += consumed;
            raw.len -= consumed;
        }

        if (result != DecodeResult::kFinished) {
            break;
        }
    }

    if (result == DecodeResult::kFinished && IsCurrentGeneration(generation) &&
        pending_pcm.size() > pending_offset) {
        std::vector<int16_t> final_chunk(pending_pcm.begin() + pending_offset, pending_pcm.end());
        if (!SubmitPcmChunk(final_chunk, generation)) {
            result = DecodeResult::kChanged;
        }
    }

    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    esp_audio_simple_dec_close(decoder);
    fclose(file);

    if (!IsCurrentGeneration(generation) && result == DecodeResult::kFinished) {
        result = DecodeResult::kChanged;
    }
    if (card_io_error) {
        MarkCardUnavailable();
    }
    return result;
}

bool SdMusicPlayer::SubmitPcmChunk(std::vector<int16_t>& chunk, uint32_t generation) {
    if (!WaitUntilPlayable(generation)) {
        return false;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    const size_t sample_count = chunk.size();
    uint64_t absolute_sum = 0;
    for (const int16_t sample : chunk) {
        absolute_sum += static_cast<uint32_t>(std::abs(static_cast<int32_t>(sample)));
    }
    const int mean_absolute_sample = sample_count == 0
        ? 0
        : static_cast<int>(absolute_sum / sample_count);
    if (!audio.PushPcmToPlaybackQueue(std::move(chunk), kAudioPlaybackSourceLocalMusic, true)) {
        return false;
    }

    bool still_current = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_ && state_ != State::kStopped) {
            submitted_samples_ += sample_count;
            still_current = true;
        }
    }
    if (!still_current) {
        // A stop or track change may race with a blocked queue push. Remove the stale frame.
        ClearQueuedMusic();
        return false;
    }
    UpdateLyrics(generation);
    UpdateProgress(generation);
    UpdatePlaybackVisualization(generation, mean_absolute_sample);
    return true;
}

bool SdMusicPlayer::WaitUntilPlayable(uint32_t generation) {
    bool queue_cleared = false;
    while (!shutting_down_) {
        State state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || state_ == State::kStopped) {
                return false;
            }
            state = state_;
        }

        const bool device_idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
        if (state == State::kPlaying && device_idle) {
            std::string title;
            std::string lyric_window;
            bool restore_text = false;
            bool still_current = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                restore_text = auto_paused_;
                auto_paused_ = false;
                still_current = generation == generation_;
                if (restore_text && still_current && current_index_ >= 0 &&
                    current_index_ < static_cast<int>(songs_.size())) {
                    title = songs_[current_index_].title;
                    const int64_t progress_ms = submitted_samples_ * 1000ULL / kOutputSampleRate;
                    int lyric_index = -1;
                    for (size_t i = 0; i < current_lyrics_.size(); ++i) {
                        if (current_lyrics_[i].time_ms > progress_ms) {
                            break;
                        }
                        lyric_index = i;
                        displayed_lyric_index_ = i;
                    }
                    lyric_window = BuildLyricsWindowLocked(lyric_index);
                }
            }
            if (restore_text && !title.empty()) {
                ShowMusicText(title, lyric_window);
                UpdateProgress(generation, true);
                UpdatePlaybackVisualization(generation, -1, true);
            }
            return still_current;
        }

        if (!queue_cleared) {
            ClearQueuedMusic();
            ClearMusicText();
            queue_cleared = true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto_paused_ = state == State::kPlaying && !device_idle;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    return false;
}

bool SdMusicPlayer::IsCurrentGeneration(uint32_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !shutting_down_ && generation == generation_ && state_ != State::kStopped;
}

std::string SdMusicPlayer::EnsureLibrary() {
    std::lock_guard<std::mutex> filesystem_lock(filesystem_mutex_);
    bool mounted;
    bool scanned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mounted = mounted_;
        scanned = scanned_;
    }
    if (!mounted) {
        const std::string mount_error = MountCard();
        if (!mount_error.empty()) {
            return mount_error;
        }
    }
    if (!scanned) {
        std::string error;
        if (!ScanLibrary(error)) {
            return error;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (songs_.empty()) {
            return "SD卡已挂载，但/music及根目录中没有找到MP3歌曲。";
        }
    }
    return {};
}

std::string SdMusicPlayer::MountCard() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CARD_CLK_GPIO;
    slot.cmd = SD_CARD_CMD_GPIO;
    slot.d0 = SD_CARD_D0_GPIO;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(kTag, "Mounting 1-bit SDMMC: CLK=%d CMD=%d D0=%d",
             SD_CARD_CLK_GPIO, SD_CARD_CMD_GPIO, SD_CARD_D0_GPIO);
    const esp_err_t result = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot,
                                                     &mount_config, &card_);
    if (result != ESP_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        card_ = nullptr;
        mounted_ = false;
        scanned_ = false;
        ESP_LOGW(kTag, "SD mount failed without formatting: %s", esp_err_to_name(result));
        return "无法挂载SD卡。请确认已插入FAT32卡；固件不会自动格式化卡。";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mounted_ = true;
        scanned_ = false;
    }
    sdmmc_card_print_info(stdout, card_);
    return {};
}

bool SdMusicPlayer::ScanLibrary(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    songs_.clear();

    if (PathExists(std::string(kMountPoint) + "/music")) {
        ScanDirectory(std::string(kMountPoint) + "/music", true);
    }
    ScanDirectory(kMountPoint, false);

    std::sort(songs_.begin(), songs_.end(), [](const Song& left, const Song& right) {
        return LowerAscii(left.title) < LowerAscii(right.title);
    });
    if (songs_.empty()) {
        error = "SD卡已挂载，但/music及根目录中没有找到MP3歌曲。";
    }
    scanned_ = true;
    ESP_LOGI(kTag, "SD music scan found %u song(s)",
             static_cast<unsigned>(songs_.size()));
    return error.empty();
}

void SdMusicPlayer::ScanDirectory(const std::string& path, bool recursive) {
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
        return;
    }
    while (songs_.size() < kMaxSongs) {
        dirent* entry = readdir(directory);
        if (entry == nullptr) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const std::string child = path + "/" + entry->d_name;
        struct stat info = {};
        if (stat(child.c_str(), &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode) && recursive) {
            ScanDirectory(child, true);
        } else if (S_ISREG(info.st_mode) && HasMp3Extension(child)) {
            AddSong(child);
        }
    }
    closedir(directory);
}

void SdMusicPlayer::AddSong(const std::string& path) {
    for (const auto& song : songs_) {
        if (song.path == path) {
            return;
        }
    }
    const size_t dot = path.find_last_of('.');
    const std::string lyric = path.substr(0, dot) + ".lrc";
    songs_.push_back({path, FileStem(path), PathExists(lyric) ? lyric : ""});
}

void SdMusicPlayer::MarkCardUnavailable() {
    std::lock_guard<std::mutex> filesystem_lock(filesystem_mutex_);
    sdmmc_card_t* card = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mounted_) {
            card = card_;
        }
        mounted_ = false;
        scanned_ = false;
        card_ = nullptr;
    }
    if (card != nullptr) {
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    }
}

std::string SdMusicPlayer::PlaySong(const std::string& requested_name) {
    if (worker_task_ == nullptr) {
        return "音乐播放任务启动失败，请重启设备。";
    }
    const std::string requested_title = NormalizeRequestedName(requested_name);
    if (!requested_title.empty()) {
        ShowMusicText(requested_title, "正在查找歌曲…");
    }
    const std::string error = EnsureLibrary();
    if (!error.empty()) {
        ClearMusicText();
        return error;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto matches = MatchSongs(requested_name);
    if (matches.size() != 1) {
        if (matches.empty()) {
            std::vector<int> suggestions;
            const size_t count = std::min<size_t>(songs_.size(), 8);
            for (size_t i = 0; i < count; ++i) {
                suggestions.push_back(i);
            }
            std::ostringstream text;
            text << "没有找到“" << NormalizeRequestedName(requested_name)
                 << "”。请从这些候选中说出完整歌名：";
            for (const int index : suggestions) {
                text << "\n- " << songs_[index].title;
            }
            ClearMusicText();
            return text.str();
        }
        ClearMusicText();
        return FormatCandidates(matches);
    }
    SelectTrackLocked(matches.front());
    const std::string title = songs_[current_index_].title;
    ShowMusicText(title, "正在加载歌词…");
    Application::GetInstance().EndConversation();
    xTaskNotifyGive(worker_task_);
    return "正在播放《" + title + "》。";
}

std::string SdMusicPlayer::Control(const std::string& action_value) {
    const std::string action = LowerAscii(Trim(action_value));
    if (action == "refresh") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
            state_ = State::kStopped;
            current_index_ = -1;
            scanned_ = false;
            playback_ui_task_pending_ = false;
            pending_playback_ui_generation_ = generation_;
        }
        if (worker_task_ != nullptr) {
            xTaskNotifyGive(worker_task_);
        }
        ClearQueuedMusic();
        ClearMusicText();
        const std::string error = EnsureLibrary();
        if (!error.empty()) {
            return error;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return "已重新扫描SD卡，共找到" + std::to_string(songs_.size()) + "首MP3。";
    }

    if (action == "pause") {
        uint32_t paused_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::kStopped) {
                return "当前没有正在播放的SD卡音乐。";
            }
            state_ = State::kPaused;
            auto_paused_ = false;
            paused_generation = generation_;
            smoothed_music_level_ = 0;
            std::fill(std::begin(music_level_history_), std::end(music_level_history_), 0);
        }
        ClearQueuedMusic();
        UpdatePlaybackVisualization(paused_generation, -1, true);
        if (worker_task_ != nullptr) {
            xTaskNotifyGive(worker_task_);
        }
        return "音乐已暂停。";
    }

    if (action == "resume") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kPaused) {
            return state_ == State::kPlaying ? "音乐已经在播放。" : "没有可继续的歌曲。";
        }
        state_ = State::kPlaying;
        if (worker_task_ != nullptr) {
            xTaskNotifyGive(worker_task_);
        }
        return "音乐将在语音交互结束后继续播放。";
    }

    if (action == "stop") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
            state_ = State::kStopped;
            current_index_ = -1;
            submitted_samples_ = 0;
            total_samples_ = 0;
            displayed_progress_permille_ = -1;
            next_playback_ui_sample_ = 0;
            smoothed_music_level_ = 0;
            std::fill(std::begin(music_level_history_), std::end(music_level_history_), 0);
            playback_ui_task_pending_ = false;
            pending_playback_ui_generation_ = generation_;
            auto_paused_ = false;
            lyrics_available_ = false;
            current_lyrics_.clear();
        }
        if (worker_task_ != nullptr) {
            xTaskNotifyGive(worker_task_);
        }
        ClearQueuedMusic();
        ClearMusicText();
        return "SD卡音乐已停止。";
    }

    if (action != "next" && action != "previous" && action != "random") {
        return "无效控制命令。可用命令：pause、resume、stop、next、previous、random、refresh。";
    }

    const std::string error = EnsureLibrary();
    if (!error.empty()) {
        return error;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    int index = current_index_;
    if (action == "random") {
        index = esp_random() % songs_.size();
        if (songs_.size() > 1 && index == current_index_) {
            index = (index + 1) % songs_.size();
        }
    } else if (action == "previous") {
        index = current_index_ <= 0 ? songs_.size() - 1 : current_index_ - 1;
    } else {
        index = current_index_ < 0 ? 0 : (current_index_ + 1) % songs_.size();
    }
    SelectTrackLocked(index);
    const std::string title = songs_[current_index_].title;
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
    return "已切换到《" + title + "》。";
}

std::string SdMusicPlayer::ListSongs(int offset, int limit) {
    const std::string error = EnsureLibrary();
    if (!error.empty()) {
        return error;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset >= static_cast<int>(songs_.size())) {
        return "offset超出歌曲列表，共" + std::to_string(songs_.size()) + "首。";
    }
    const int end = std::min<int>(songs_.size(), offset + limit);
    std::ostringstream text;
    text << "SD卡歌曲 " << (offset + 1) << "-" << end << "/" << songs_.size() << "：";
    for (int i = offset; i < end; ++i) {
        text << "\n" << (i + 1) << ". " << songs_[i].title;
    }
    return text.str();
}

void SdMusicPlayer::SelectTrackLocked(int index) {
    ++generation_;
    current_index_ = index;
    state_ = State::kPlaying;
    submitted_samples_ = 0;
    total_samples_ = 0;
    displayed_progress_permille_ = -1;
    next_playback_ui_sample_ = 0;
    smoothed_music_level_ = 0;
    std::fill(std::begin(music_level_history_), std::end(music_level_history_), 0);
    playback_ui_task_pending_ = false;
    pending_playback_ui_generation_ = generation_;
    auto_paused_ = false;
    lyrics_available_ = false;
    displayed_lyric_index_ = -1;
    current_lyrics_.clear();
}

std::vector<int> SdMusicPlayer::MatchSongs(const std::string& requested_name) const {
    const std::string query = LowerAscii(NormalizeRequestedName(requested_name));
    if (query.empty()) {
        return {};
    }
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<int> matches;
        for (size_t i = 0; i < songs_.size(); ++i) {
            const std::string title = LowerAscii(songs_[i].title);
            const bool match = pass == 0 ? title == query
                               : pass == 1 ? title.rfind(query, 0) == 0
                                           : title.find(query) != std::string::npos;
            if (match) {
                matches.push_back(i);
            }
        }
        if (!matches.empty()) {
            return matches;
        }
    }
    return {};
}

std::string SdMusicPlayer::FormatCandidates(const std::vector<int>& matches) const {
    std::ostringstream text;
    text << "匹配到多首歌曲，请说出更完整的歌名：";
    const size_t count = std::min<size_t>(matches.size(), 8);
    for (size_t i = 0; i < count; ++i) {
        text << "\n- " << songs_[matches[i]].title;
    }
    if (matches.size() > count) {
        text << "\n另有" << (matches.size() - count) << "首。";
    }
    return text.str();
}

std::vector<SdMusicPlayer::LyricLine> SdMusicPlayer::LoadLyrics(const std::string& path) {
    std::vector<LyricLine> lyrics;
    if (path.empty()) {
        return lyrics;
    }
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return lyrics;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return lyrics;
    }
    const long file_size = ftell(file);
    if (file_size <= 0 || static_cast<size_t>(file_size) > kMaxLrcFileSize ||
        fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGW(kTag, "Invalid LRC size for %s: %ld", path.c_str(), file_size);
        fclose(file);
        return lyrics;
    }
    std::string raw(static_cast<size_t>(file_size), '\0');
    const size_t bytes_read = fread(raw.data(), 1, raw.size(), file);
    fclose(file);
    raw.resize(bytes_read);
    if (raw.empty()) {
        return lyrics;
    }

    const std::string decoded = DecodeLrcContent(std::move(raw));
    std::istringstream lines(decoded);
    int64_t offset_ms = 0;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::string lower = LowerAscii(line);
        if (lower.rfind("[offset:", 0) == 0) {
            const size_t close = lower.find(']');
            if (close != std::string::npos) {
                offset_ms = strtoll(lower.substr(8, close - 8).c_str(), nullptr, 10);
            }
            continue;
        }

        std::vector<int64_t> timestamps;
        size_t position = 0;
        while (position < line.size() && line[position] == '[') {
            const size_t close = line.find(']', position + 1);
            if (close == std::string::npos) {
                break;
            }
            int64_t timestamp = 0;
            if (!ParseTimestamp(line.substr(position + 1, close - position - 1), timestamp)) {
                break;
            }
            timestamps.push_back(timestamp);
            position = close + 1;
        }
        const std::string lyric_text = Trim(line.substr(position));
        if (lyric_text.empty()) {
            continue;
        }
        for (const auto timestamp : timestamps) {
            lyrics.push_back({timestamp, lyric_text});
        }
    }
    for (auto& lyric : lyrics) {
        lyric.time_ms = std::max<int64_t>(0, lyric.time_ms + offset_ms);
    }
    std::stable_sort(lyrics.begin(), lyrics.end(), [](const LyricLine& left, const LyricLine& right) {
        return left.time_ms < right.time_ms;
    });
    return lyrics;
}

std::string SdMusicPlayer::BuildLyricsWindowLocked(int current_index) const {
    if (current_lyrics_.empty()) {
        return "暂无同步歌词";
    }

    const int lyric_count = static_cast<int>(current_lyrics_.size());
    const int anchor = current_index >= 0 && current_index < lyric_count ? current_index : 0;
    int start = anchor;
    int end = anchor;
    int used_rows = EstimateLyricsRows(current_lyrics_[anchor].text, current_index == anchor);

    if (current_index > 0) {
        const int previous_rows = EstimateLyricsRows(current_lyrics_[current_index - 1].text, false);
        if (used_rows + previous_rows <= kLyricsMaxVisualRows) {
            start = current_index - 1;
            used_rows += previous_rows;
        }
    }

    while (end + 1 < lyric_count) {
        const int next_rows = EstimateLyricsRows(current_lyrics_[end + 1].text, false);
        if (used_rows + next_rows > kLyricsMaxVisualRows) {
            break;
        }
        ++end;
        used_rows += next_rows;
    }

    while (start > 0) {
        const int previous_rows = EstimateLyricsRows(current_lyrics_[start - 1].text, false);
        if (used_rows + previous_rows > kLyricsMaxVisualRows) {
            break;
        }
        --start;
        used_rows += previous_rows;
    }

    std::ostringstream window;
    for (int index = start; index <= end; ++index) {
        if (index == current_index) {
            window << "> ";
        }
        window << current_lyrics_[index].text;
        if (index != end) {
            window << '\n';
        }
    }
    return window.str();
}

void SdMusicPlayer::UpdateLyrics(uint32_t generation) {
    std::string title;
    std::string lyric_window;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || current_index_ < 0 ||
            current_index_ >= static_cast<int>(songs_.size()) || current_lyrics_.empty()) {
            return;
        }
        const int64_t progress_ms = submitted_samples_ * 1000ULL / kOutputSampleRate;
        int index = -1;
        for (size_t i = 0; i < current_lyrics_.size(); ++i) {
            if (current_lyrics_[i].time_ms > progress_ms) {
                break;
            }
            index = i;
        }
        if (index < 0 || index == displayed_lyric_index_) {
            return;
        }
        displayed_lyric_index_ = index;
        title = songs_[current_index_].title;
        lyric_window = BuildLyricsWindowLocked(index);
    }
    ShowMusicText(title, lyric_window);
}

void SdMusicPlayer::UpdateProgress(uint32_t generation, bool force) {
    static constexpr int kProgressUiStepPermille = 5;
    int progress_permille = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || state_ == State::kStopped || total_samples_ == 0) {
            return;
        }
        progress_permille = static_cast<int>(std::min<uint64_t>(
            1000, submitted_samples_ * 1000ULL / total_samples_));
        if (!force && displayed_progress_permille_ >= 0 && progress_permille < 1000 &&
            progress_permille - displayed_progress_permille_ < kProgressUiStepPermille) {
            return;
        }
        displayed_progress_permille_ = progress_permille;
    }
    // Rendering the transparent 360x185 arc can occasionally take longer than
    // the ~80 ms local-music PCM queue. Keep it off the decoder task so display
    // work can never starve audio submission.
    Application::GetInstance().Schedule([this, generation, progress_permille]() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || state_ != State::kPlaying || auto_paused_ ||
                displayed_progress_permille_ != progress_permille) {
                return;
            }
        }
        if (auto* display = Board::GetInstance().GetDisplay()) {
            display->SetMusicProgress(progress_permille);
        }
    });
}

void SdMusicPlayer::UpdatePlaybackVisualization(uint32_t generation,
                                                int mean_absolute_sample,
                                                bool force) {
    static constexpr uint64_t kUiIntervalSamples = kOutputSampleRate / 10;
    bool schedule_task = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || state_ == State::kStopped) {
            return;
        }

        if (mean_absolute_sample >= 0) {
            const int raw_level = std::clamp(
                (mean_absolute_sample - 200) * 1000 / 7000, 0, 1000);
            if (raw_level >= smoothed_music_level_) {
                smoothed_music_level_ = (smoothed_music_level_ + raw_level * 3) / 4;
            } else {
                smoothed_music_level_ = (smoothed_music_level_ * 3 + raw_level) / 4;
            }
        }

        if (!force && submitted_samples_ < next_playback_ui_sample_) {
            return;
        }
        next_playback_ui_sample_ = submitted_samples_ + kUiIntervalSamples;
        music_level_history_[0] = music_level_history_[1];
        music_level_history_[1] = music_level_history_[2];
        music_level_history_[2] = smoothed_music_level_;

        pending_elapsed_seconds_ = static_cast<int>(submitted_samples_ / kOutputSampleRate);
        pending_total_seconds_ = static_cast<int>(total_samples_ / kOutputSampleRate);
        std::copy(std::begin(music_level_history_), std::end(music_level_history_),
                  std::begin(pending_music_levels_));
        pending_playback_ui_generation_ = generation;
        if (!playback_ui_task_pending_) {
            playback_ui_task_pending_ = true;
            schedule_task = true;
        }
    }

    if (!schedule_task) {
        return;
    }
    Application::GetInstance().Schedule([this, generation]() {
        int elapsed_seconds = 0;
        int total_seconds = 0;
        int levels[3] = {};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_playback_ui_generation_ != generation) {
                return;
            }
            playback_ui_task_pending_ = false;
            if (generation != generation_ || state_ == State::kStopped || auto_paused_) {
                return;
            }
            elapsed_seconds = pending_elapsed_seconds_;
            total_seconds = pending_total_seconds_;
            std::copy(std::begin(pending_music_levels_), std::end(pending_music_levels_),
                      std::begin(levels));
        }
        if (auto* display = Board::GetInstance().GetDisplay()) {
            display->SetMusicPlaybackInfo(elapsed_seconds, total_seconds,
                                          levels[0], levels[1], levels[2]);
        }
    });
}

void SdMusicPlayer::ShowMusicText(const std::string& title, const std::string& lyric) {
    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->SetMusicLyrics(title.c_str(), lyric.c_str());
    }
}

void SdMusicPlayer::ClearMusicText() {
    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->ClearMusicLyrics();
    }
}

size_t SdMusicPlayer::ClearQueuedMusic() {
    const size_t cleared = Application::GetInstance().GetAudioService().ClearPlaybackQueue(
        kAudioPlaybackSourceLocalMusic);
    if (cleared > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        submitted_samples_ = cleared >= submitted_samples_ ? 0 : submitted_samples_ - cleared;
        displayed_lyric_index_ = -1;
        displayed_progress_permille_ = -1;
    }
    return cleared;
}

std::string SdMusicPlayer::Trim(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string SdMusicPlayer::LowerAscii(const std::string& value) {
    std::string result = value;
    for (char& character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x80) {
            character = static_cast<char>(std::tolower(byte));
        }
    }
    return result;
}

std::string SdMusicPlayer::NormalizeRequestedName(const std::string& value) {
    std::string result = Trim(value);
    const std::string left_bracket = "《";
    const std::string right_bracket = "》";
    if (result.rfind(left_bracket, 0) == 0 && result.size() >= left_bracket.size()) {
        result.erase(0, left_bracket.size());
    }
    if (result.size() >= right_bracket.size() &&
        result.compare(result.size() - right_bracket.size(), right_bracket.size(), right_bracket) == 0) {
        result.erase(result.size() - right_bracket.size());
    }
    result = Trim(result);
    if (HasMp3Extension(result)) {
        result.erase(result.size() - 4);
    }
    return Trim(result);
}

bool SdMusicPlayer::HasMp3Extension(const std::string& path) {
    if (path.size() < 4) {
        return false;
    }
    return LowerAscii(path.substr(path.size() - 4)) == ".mp3";
}

std::string SdMusicPlayer::FileStem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = path.find_last_of('.');
    const size_t end = dot == std::string::npos || dot < start ? path.size() : dot;
    return Trim(path.substr(start, end - start));
}

bool SdMusicPlayer::ParseTimestamp(const std::string& token, int64_t& milliseconds) {
    const size_t colon = token.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size()) {
        return false;
    }
    char* end = nullptr;
    const long minutes = strtol(token.substr(0, colon).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || minutes < 0) {
        return false;
    }
    const std::string seconds_text = token.substr(colon + 1);
    char* seconds_end = nullptr;
    const double seconds = strtod(seconds_text.c_str(), &seconds_end);
    if (seconds_end == nullptr || *seconds_end != '\0' || seconds < 0 || seconds >= 60) {
        return false;
    }
    milliseconds = static_cast<int64_t>(minutes) * 60000 +
                   static_cast<int64_t>(seconds * 1000.0 + 0.5);
    return true;
}
