#pragma once

#include <functional>
#include <mutex>
#include <string>

#include <esp_http_server.h>
#include <esp_timer.h>

class MediaTransferServer {
public:
    using RefreshCallback = std::function<void()>;
    using DisplayCallback = std::function<void(const std::string&)>;

    MediaTransferServer(RefreshCallback refresh_wallpapers, DisplayCallback show_qr,
                        DisplayCallback hide_qr);
    ~MediaTransferServer();

    std::string Start();
    std::string Stop();
    std::string Status() const;

private:
    static void TimeoutCallback(void* arg);
    static esp_err_t GetHandler(httpd_req_t* req);
    static esp_err_t UploadHandler(httpd_req_t* req);
    static esp_err_t DeleteHandler(httpd_req_t* req);
    static esp_err_t RenameHandler(httpd_req_t* req);

    bool IsValidName(const std::string& name) const;
    bool IsWallpaperFile(const std::string& name) const;
    std::string WallpaperDirectory() const;
    std::string ReadQuery(httpd_req_t* req, const char* key) const;
    void SendJson(httpd_req_t* req, int status, const std::string& body) const;
    void SendError(httpd_req_t* req, int status, const char* message) const;
    std::string ListWallpapers() const;
    void StopLocked();

    RefreshCallback refresh_wallpapers_;
    DisplayCallback show_qr_;
    DisplayCallback hide_qr_;
    mutable std::mutex mutex_;
    httpd_handle_t server_ = nullptr;
    esp_timer_handle_t timeout_timer_ = nullptr;
    std::string url_;
};
