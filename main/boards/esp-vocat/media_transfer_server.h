#pragma once

#include <functional>
#include <mutex>
#include <string>

#include <esp_http_server.h>
#include <esp_timer.h>

class MediaTransferServer {
public:
    struct Snapshot {
        bool running = false;
        bool touch_owned = false;
        std::string url;
    };
    using RefreshCallback = std::function<void()>;
    using BusyCallback = std::function<bool()>;
    using DisplayCallback = std::function<void(const std::string&)>;

    MediaTransferServer(RefreshCallback refresh_wallpapers, RefreshCallback refresh_music,
                        BusyCallback music_active, DisplayCallback show_qr, DisplayCallback hide_qr);
    ~MediaTransferServer();

    std::string Start();
    std::string StartForTouch();
    std::string Stop();
    std::string StopForTouch();
    std::string Status() const;
    Snapshot GetSnapshot() const;

private:
    static void TimeoutCallback(void* arg);
    static esp_err_t GetHandler(httpd_req_t* req);
    static esp_err_t UploadHandler(httpd_req_t* req);
    static esp_err_t DeleteHandler(httpd_req_t* req);
    static esp_err_t RenameHandler(httpd_req_t* req);

    enum class FileType { kWallpaper, kMusic };

    bool ParseFileType(httpd_req_t* req, FileType& type) const;
    bool IsValidName(const std::string& name) const;
    bool IsSupportedFile(const std::string& name, FileType type) const;
    size_t MaxFileSize(FileType type, const std::string& name) const;
    std::string Directory(FileType type) const;
    std::string ReadQuery(httpd_req_t* req, const char* key) const;
    void SendJson(httpd_req_t* req, int status, const std::string& body) const;
    void SendError(httpd_req_t* req, int status, const char* message) const;
    std::string ListFiles(FileType type) const;
    std::string StartLocked(bool touch_owned, bool show_qr);
    void StopLocked(bool hide_qr = true);

    RefreshCallback refresh_wallpapers_;
    RefreshCallback refresh_music_;
    BusyCallback music_active_;
    DisplayCallback show_qr_;
    DisplayCallback hide_qr_;
    mutable std::mutex mutex_;
    httpd_handle_t server_ = nullptr;
    esp_timer_handle_t timeout_timer_ = nullptr;
    std::string url_;
    bool touch_owned_ = false;
};
