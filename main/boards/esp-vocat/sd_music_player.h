#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>

class SdMusicPlayer {
public:
    SdMusicPlayer();
    ~SdMusicPlayer();

    void RegisterMcpTools();

private:
    struct Song {
        std::string path;
        std::string title;
        std::string lyric_path;
    };

    struct LyricLine {
        int64_t time_ms;
        std::string text;
    };

    enum class State {
        kStopped,
        kPlaying,
        kPaused,
    };

    enum class DecodeResult {
        kFinished,
        kChanged,
        kError,
    };

    static constexpr size_t kMaxSongs = 256;
    static constexpr uint32_t kOutputSampleRate = 24000;
    static constexpr size_t kPcmChunkSamples = 960;

    std::mutex mutex_;
    std::mutex filesystem_mutex_;
    std::vector<Song> songs_;
    sdmmc_card_t* card_ = nullptr;
    bool mounted_ = false;
    bool scanned_ = false;
    State state_ = State::kStopped;
    int current_index_ = -1;
    uint32_t generation_ = 0;
    uint64_t submitted_samples_ = 0;
    bool auto_paused_ = false;
    bool lyrics_available_ = false;
    int displayed_lyric_index_ = -1;
    std::vector<LyricLine> current_lyrics_;
    TaskHandle_t worker_task_ = nullptr;
    std::atomic<bool> shutting_down_{false};

    static void WorkerTaskEntry(void* arg);
    void WorkerTask();
    DecodeResult DecodeSong(const Song& song, uint32_t generation);
    bool SubmitPcmChunk(std::vector<int16_t>& chunk, uint32_t generation);
    bool WaitUntilPlayable(uint32_t generation);
    bool IsCurrentGeneration(uint32_t generation);

    std::string EnsureLibrary();
    std::string MountCard();
    bool ScanLibrary(std::string& error);
    void ScanDirectory(const std::string& path, bool recursive);
    void AddSong(const std::string& path);
    void MarkCardUnavailable();

    std::string PlaySong(const std::string& requested_name);
    std::string Control(const std::string& action);
    std::string ListSongs(int offset, int limit);
    void SelectTrackLocked(int index);
    std::vector<int> MatchSongs(const std::string& requested_name) const;
    std::string FormatCandidates(const std::vector<int>& matches) const;

    std::vector<LyricLine> LoadLyrics(const std::string& path);
    std::string BuildLyricsWindowLocked(int current_index) const;
    void UpdateLyrics(uint32_t generation);
    void ShowMusicText(const std::string& title, const std::string& lyric = "");
    void ClearMusicText();
    size_t ClearQueuedMusic();

    static std::string Trim(const std::string& value);
    static std::string LowerAscii(const std::string& value);
    static std::string NormalizeRequestedName(const std::string& value);
    static bool HasMp3Extension(const std::string& path);
    static std::string FileStem(const std::string& path);
    static bool ParseTimestamp(const std::string& token, int64_t& milliseconds);
};
