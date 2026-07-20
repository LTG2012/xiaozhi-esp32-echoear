#include "media_transfer_server.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include <esp_log.h>
#include <wifi_manager.h>

#include "boards/common/sd_card_manager.h"

namespace {
constexpr char kTag[] = "WallpaperServer";
constexpr int64_t kSessionUs = 10LL * 60 * 1000 * 1000;
constexpr size_t kWallpaperLimit = 2 * 1024 * 1024;
constexpr char kHtml[] = R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>小智壁纸管理</title><style>:root{font-family:Inter,system-ui,-apple-system,"Microsoft YaHei",sans-serif;color:#eaf0ff;background:#07111f}*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 15% 0,#244a7a 0,transparent 35%),linear-gradient(145deg,#07111f,#111c31)}main{max-width:760px;margin:auto;padding:32px 18px 54px}.hero{padding:24px;border:1px solid #ffffff22;border-radius:24px;background:#ffffff0c;backdrop-filter:blur(18px);box-shadow:0 24px 70px #0006}h1{margin:0;font-size:28px;letter-spacing:.03em}.sub{color:#afbed9;line-height:1.65}.pill{display:inline-block;margin-top:8px;padding:6px 11px;border-radius:999px;color:#9fe5ff;background:#1483aa33;font-size:13px}.card{margin-top:18px;padding:20px;border-radius:20px;background:#0d192cdd;border:1px solid #ffffff18}.drop{display:block;border:1.5px dashed #668bc5;border-radius:16px;padding:26px;text-align:center;cursor:pointer;transition:.2s}.drop:hover{border-color:#76d8ff;background:#76d8ff0d}.drop strong{display:block;font-size:17px}.drop span{display:block;margin-top:7px;color:#a7b5ca;font-size:13px}input{display:none}button{border:0;border-radius:11px;padding:11px 16px;font:inherit;font-weight:650;cursor:pointer;background:#57c8ff;color:#03111e}button:hover{filter:brightness(1.08)}button.secondary{background:#ffffff12;color:#eaf0ff}button.danger{background:#ff657833;color:#ffb7c1}.actions{display:flex;gap:10px;margin-top:14px;align-items:center}.progress{height:9px;margin-top:16px;border-radius:99px;background:#ffffff12;overflow:hidden}.bar{height:100%;width:0;background:linear-gradient(90deg,#55c8ff,#8b8cff);transition:width .14s}.status{min-height:24px;margin:12px 0 0;color:#a7b5ca}.status.error{color:#ffabb6}.section{display:flex;justify-content:space-between;align-items:center;margin-top:28px}.list{margin:12px 0 0;padding:0;list-style:none}.file{display:flex;align-items:center;gap:12px;padding:14px 4px;border-bottom:1px solid #ffffff12}.thumb{width:38px;height:38px;border-radius:12px;background:linear-gradient(145deg,#51c9ff,#7364d9)}.name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.size{color:#91a2bb;font-size:13px}.empty{padding:30px 0;text-align:center;color:#8fa0b9}@media(max-width:480px){main{padding:18px 12px}.file{gap:7px}.file button{padding:8px 10px}}</style></head><body><main><section class="hero"><h1>壁纸管理</h1><p class="sub">通过局域网直接管理设备 SD 卡中的自定义壁纸。上传完成后会自动加入轮播。</p><span class="pill">JPEG · 最大 2 MiB · 最大 1024 × 1024</span></section><section class="card"><label class="drop" for="file"><strong>选择一张壁纸</strong><span id="picked">JPG 或 JPEG 文件</span></label><input id="file" type="file" accept="image/jpeg,.jpg,.jpeg"><div class="actions"><button id="upload">上传壁纸</button><button class="secondary" id="refresh">刷新列表</button></div><div class="progress"><div class="bar" id="bar"></div></div><div class="status" id="status">准备就绪</div></section><div class="section"><h2>当前壁纸</h2><span id="count" class="pill">0 张</span></div><ul class="list" id="list"></ul></main><script>const $=s=>document.querySelector(s),file=$('#file'),status=$('#status'),bar=$('#bar');const say=(text,error=false)=>{status.textContent=text;status.className='status'+(error?' error':'')};const size=n=>n<1024*1024?(n/1024).toFixed(0)+' KB':(n/1024/1024).toFixed(2)+' MB';async function api(path,opt={}){const r=await fetch(path,opt),j=await r.json().catch(()=>({error:'设备响应无效'}));if(!r.ok)throw Error(j.error||'请求失败');return j}async function load(){try{const j=await api('/api/list');const list=$('#list');list.innerHTML='';$('#count').textContent=j.files.length+' 张';if(!j.files.length){list.innerHTML='<li class="empty">还没有自定义壁纸</li>';return}j.files.forEach(f=>{const li=document.createElement('li');li.className='file';li.innerHTML='<span class="thumb"></span><span class="name" title=""></span><span class="size"></span>';li.querySelector('.name').textContent=f.name;li.querySelector('.name').title=f.name;li.querySelector('.size').textContent=size(f.size);const rename=document.createElement('button');rename.className='secondary';rename.textContent='重命名';rename.onclick=async()=>{const name=prompt('新文件名（保留 .jpg 或 .jpeg）',f.name);if(name){try{await api('/api/rename?from='+encodeURIComponent(f.name)+'&to='+encodeURIComponent(name),{method:'POST'});say('已重命名');load()}catch(e){say(e.message,true)}}};const del=document.createElement('button');del.className='danger';del.textContent='删除';del.onclick=async()=>{if(confirm('确定删除“'+f.name+'”吗？')){try{await api('/api/file?name='+encodeURIComponent(f.name),{method:'DELETE'});say('已删除');load()}catch(e){say(e.message,true)}}};li.append(rename,del);list.append(li)})}catch(e){say(e.message,true)}}file.onchange=()=>{const f=file.files[0];$('#picked').textContent=f?f.name+' · '+size(f.size):'JPG 或 JPEG 文件'};$('#refresh').onclick=load;$('#upload').onclick=()=>{const f=file.files[0];if(!f)return say('请先选择一张壁纸',true);if(f.size>2097152)return say('文件超过 2 MiB 限制',true);bar.style.width='0%';say('正在上传…');const xhr=new XMLHttpRequest();xhr.open('PUT','/api/upload?name='+encodeURIComponent(f.name));xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);bar.style.width=p+'%';say('正在上传 '+p+'%')}};xhr.onload=()=>{let j={};try{j=JSON.parse(xhr.responseText)}catch{}if(xhr.status>=200&&xhr.status<300){bar.style.width='100%';say('上传完成，已刷新壁纸轮播');file.value='';$('#picked').textContent='JPG 或 JPEG 文件';load()}else say(j.error||'上传失败',true)};xhr.onerror=()=>say('网络连接中断',true);xhr.send(f)};load();</script></body></html>)HTML";

std::string JsonEscape(const std::string& text) { std::string out; for (unsigned char c : text) { if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); } else if (c >= 0x20) out += static_cast<char>(c); } return out; }
std::string Lower(std::string value) { std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); }); return value; }
bool HasSuffix(const std::string& value, const char* suffix) { const size_t length = strlen(suffix); return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0; }
bool PathEquals(const char* uri, const char* path) { const char* query = uri ? strchr(uri, '?') : nullptr; const size_t length = query ? static_cast<size_t>(query - uri) : (uri ? strlen(uri) : 0); return strlen(path) == length && strncmp(uri, path, length) == 0; }
std::string UrlDecode(const std::string& value) { std::string decoded; decoded.reserve(value.size()); for (size_t i = 0; i < value.size(); ++i) { if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(value[i + 1]) && std::isxdigit(value[i + 2])) { const auto hex = [](unsigned char c) { return std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10; }; decoded += static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2])); i += 2; } else decoded += value[i] == '+' ? ' ' : value[i]; } return decoded; }
bool ReadJpegDimensions(FILE* file, uint16_t& width, uint16_t& height) {
    width = 0;
    height = 0;
    if (file == nullptr || std::fgetc(file) != 0xFF || std::fgetc(file) != 0xD8) return false;
    while (true) {
        int marker = std::fgetc(file);
        while (marker == 0xFF) marker = std::fgetc(file);
        if (marker == EOF || marker == 0xD9 || marker == 0xDA) return false;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        const int length_hi = std::fgetc(file), length_lo = std::fgetc(file);
        if (length_hi == EOF || length_lo == EOF) return false;
        const int segment_length = (length_hi << 8) | length_lo;
        if (segment_length < 2) return false;
        const bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (!is_sof) {
            if (std::fseek(file, segment_length - 2, SEEK_CUR) != 0) return false;
            continue;
        }
        if (segment_length < 8 || std::fgetc(file) == EOF) return false;
        const int height_hi = std::fgetc(file), height_lo = std::fgetc(file);
        const int width_hi = std::fgetc(file), width_lo = std::fgetc(file);
        if (height_hi == EOF || height_lo == EOF || width_hi == EOF || width_lo == EOF) return false;
        height = static_cast<uint16_t>((height_hi << 8) | height_lo);
        width = static_cast<uint16_t>((width_hi << 8) | width_lo);
        return width > 0 && height > 0;
    }
}
}

MediaTransferServer::MediaTransferServer(RefreshCallback refresh_wallpapers, DisplayCallback show_qr, DisplayCallback hide_qr)
    : refresh_wallpapers_(std::move(refresh_wallpapers)), show_qr_(std::move(show_qr)), hide_qr_(std::move(hide_qr)) {
    const esp_timer_create_args_t args = {.callback = TimeoutCallback, .arg = this, .dispatch_method = ESP_TIMER_TASK, .name = "wallpaper_web", .skip_unhandled_events = true};
    ESP_ERROR_CHECK(esp_timer_create(&args, &timeout_timer_));
}

MediaTransferServer::~MediaTransferServer() { std::lock_guard<std::mutex> lock(mutex_); StopLocked(); if (timeout_timer_) esp_timer_delete(timeout_timer_); }

std::string MediaTransferServer::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_) return "Wallpaper server is already open: " + url_;
    if (!WifiManager::GetInstance().IsConnected()) return "Wi-Fi is not connected.";
    const std::string mount_error = SdCardManager::GetInstance().EnsureMounted();
    if (!mount_error.empty()) return mount_error;
    { std::lock_guard<std::mutex> filesystem_lock(SdCardManager::GetInstance().FilesystemMutex()); const std::string directory = WallpaperDirectory(); if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) return "Unable to create /wallpapers on the SD card."; }
    url_ = "http://" + WifiManager::GetInstance().GetIpAddress() + ":8080";
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.server_port = 8080; config.ctrl_port = 32770; config.stack_size = 8192; config.max_open_sockets = 4; config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server_, &config) != ESP_OK) { server_ = nullptr; url_.clear(); return "Unable to start the local wallpaper server."; }
    const httpd_uri_t routes[] = {{.uri = "/*", .method = HTTP_GET, .handler = GetHandler, .user_ctx = this}, {.uri = "/api/upload", .method = HTTP_PUT, .handler = UploadHandler, .user_ctx = this}, {.uri = "/api/file", .method = HTTP_DELETE, .handler = DeleteHandler, .user_ctx = this}, {.uri = "/api/rename", .method = HTTP_POST, .handler = RenameHandler, .user_ctx = this}};
    for (const auto& route : routes) httpd_register_uri_handler(server_, &route);
    esp_timer_start_once(timeout_timer_, kSessionUs); show_qr_(url_); ESP_LOGI(kTag, "Wallpaper server started: %s", url_.c_str());
    return "Wallpaper server is open for 10 minutes. Open " + url_ + " on a phone or computer.";
}

std::string MediaTransferServer::Stop() { std::lock_guard<std::mutex> lock(mutex_); if (!server_) return "Wallpaper server is already closed."; StopLocked(); return "Wallpaper server closed."; }
std::string MediaTransferServer::Status() const { std::lock_guard<std::mutex> lock(mutex_); return server_ ? "Wallpaper server is open: " + url_ : "Wallpaper server is closed."; }
void MediaTransferServer::TimeoutCallback(void* arg) { static_cast<MediaTransferServer*>(arg)->Stop(); }
void MediaTransferServer::StopLocked() { if (timeout_timer_) esp_timer_stop(timeout_timer_); if (server_) { httpd_stop(server_); server_ = nullptr; } url_.clear(); hide_qr_(""); }

std::string MediaTransferServer::ReadQuery(httpd_req_t* req, const char* key) const { const size_t length = httpd_req_get_url_query_len(req); if (!length || length > 512) return {}; std::string query(length + 1, '\0'), value(512, '\0'); if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK || httpd_query_key_value(query.c_str(), key, value.data(), value.size()) != ESP_OK) return {}; value.resize(strlen(value.c_str())); return UrlDecode(value); }
bool MediaTransferServer::IsValidName(const std::string& name) const { return !name.empty() && name.size() <= 120 && name.find("..") == std::string::npos && name.find_first_of("/\\\r\n") == std::string::npos; }
bool MediaTransferServer::IsWallpaperFile(const std::string& name) const { const std::string lower = Lower(name); return HasSuffix(lower, ".jpg") || HasSuffix(lower, ".jpeg"); }
std::string MediaTransferServer::WallpaperDirectory() const { return std::string(SdCardManager::GetInstance().MountPoint()) + "/wallpapers"; }
void MediaTransferServer::SendJson(httpd_req_t* req, int status, const std::string& body) const { httpd_resp_set_type(req, "application/json; charset=utf-8"); httpd_resp_set_status(req, status == 200 ? "200 OK" : status == 400 ? "400 Bad Request" : status == 404 ? "404 Not Found" : status == 409 ? "409 Conflict" : "500 Internal Server Error"); httpd_resp_send(req, body.c_str(), body.size()); }
void MediaTransferServer::SendError(httpd_req_t* req, int status, const char* message) const { SendJson(req, status, std::string("{\"error\":\"") + JsonEscape(message) + "\"}"); }

std::string MediaTransferServer::ListWallpapers() const { std::lock_guard<std::mutex> filesystem_lock(SdCardManager::GetInstance().FilesystemMutex()); DIR* handle = opendir(WallpaperDirectory().c_str()); if (!handle) return "{\"files\":[]}"; std::vector<std::pair<std::string, size_t>> files; while (dirent* entry = readdir(handle)) { const std::string name = entry->d_name; if (!IsValidName(name) || !IsWallpaperFile(name)) continue; struct stat info = {}; if (stat((WallpaperDirectory() + "/" + name).c_str(), &info) == 0 && S_ISREG(info.st_mode)) files.emplace_back(name, info.st_size); } closedir(handle); std::sort(files.begin(), files.end()); std::string json = "{\"files\":["; for (size_t i = 0; i < files.size(); ++i) { if (i) json += ','; json += "{\"name\":\"" + JsonEscape(files[i].first) + "\",\"size\":" + std::to_string(files[i].second) + '}'; } return json + "]}"; }

esp_err_t MediaTransferServer::GetHandler(httpd_req_t* req) { auto* self = static_cast<MediaTransferServer*>(req->user_ctx); ESP_LOGI(kTag, "GET %s", req->uri); if (PathEquals(req->uri, "/api/list")) { self->SendJson(req, 200, self->ListWallpapers()); return ESP_OK; } if (PathEquals(req->uri, "/")) { httpd_resp_set_type(req, "text/html; charset=utf-8"); httpd_resp_send(req, kHtml, HTTPD_RESP_USE_STRLEN); return ESP_OK; } self->SendError(req, 404, "Not found."); return ESP_OK; }

esp_err_t MediaTransferServer::UploadHandler(httpd_req_t* req) { auto* self = static_cast<MediaTransferServer*>(req->user_ctx); const std::string name = self->ReadQuery(req, "name"); if (!self->IsValidName(name) || !self->IsWallpaperFile(name)) { self->SendError(req, 400, "Only .jpg and .jpeg wallpaper files are supported."); return ESP_OK; } if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > kWallpaperLimit) { self->SendError(req, 400, "File is empty or exceeds the 2 MiB size limit."); return ESP_OK; } if (!SdCardManager::GetInstance().EnsureMounted().empty()) { self->SendError(req, 500, "SD card is unavailable."); return ESP_OK; } const std::string target = self->WallpaperDirectory() + "/" + name, part = target + ".part"; { std::lock_guard<std::mutex> filesystem_lock(SdCardManager::GetInstance().FilesystemMutex()); if (FILE* existing = fopen(target.c_str(), "rb")) { fclose(existing); self->SendError(req, 409, "A file with this name already exists."); return ESP_OK; } FILE* file = fopen(part.c_str(), "wb"); if (!file) { self->SendError(req, 500, "Cannot create file on SD card."); return ESP_OK; } std::array<char, 1024> buffer{}; int remaining = req->content_len; bool ok = true; while (remaining > 0) { const int received = httpd_req_recv(req, buffer.data(), std::min<int>(remaining, buffer.size())); if (received <= 0 || fwrite(buffer.data(), 1, received, file) != static_cast<size_t>(received)) { ok = false; break; } remaining -= received; } fclose(file); uint16_t width = 0, height = 0; FILE* validation = ok && remaining == 0 ? fopen(part.c_str(), "rb") : nullptr; const bool valid_jpeg = validation != nullptr && ReadJpegDimensions(validation, width, height) && width <= 1024 && height <= 1024; if (validation) fclose(validation); if (!valid_jpeg || rename(part.c_str(), target.c_str()) != 0) { remove(part.c_str()); self->SendError(req, 400, "JPEG is invalid or exceeds the 1024 x 1024 pixel limit."); return ESP_OK; } } self->refresh_wallpapers_(); ESP_LOGI(kTag, "Uploaded wallpaper: %s", name.c_str()); self->SendJson(req, 200, "{\"ok\":true}"); return ESP_OK; }

esp_err_t MediaTransferServer::DeleteHandler(httpd_req_t* req) { auto* self = static_cast<MediaTransferServer*>(req->user_ctx); const std::string name = self->ReadQuery(req, "name"); if (!self->IsValidName(name) || !self->IsWallpaperFile(name)) { self->SendError(req, 400, "Invalid wallpaper file."); return ESP_OK; } { std::lock_guard<std::mutex> filesystem_lock(SdCardManager::GetInstance().FilesystemMutex()); if (remove((self->WallpaperDirectory() + "/" + name).c_str()) != 0) { self->SendError(req, 404, "File was not found."); return ESP_OK; } } self->refresh_wallpapers_(); self->SendJson(req, 200, "{\"ok\":true}"); return ESP_OK; }

esp_err_t MediaTransferServer::RenameHandler(httpd_req_t* req) { auto* self = static_cast<MediaTransferServer*>(req->user_ctx); const std::string from = self->ReadQuery(req, "from"), to = self->ReadQuery(req, "to"); if (!self->IsValidName(from) || !self->IsValidName(to) || !self->IsWallpaperFile(from) || !self->IsWallpaperFile(to)) { self->SendError(req, 400, "Wallpaper names must end in .jpg or .jpeg."); return ESP_OK; } { std::lock_guard<std::mutex> filesystem_lock(SdCardManager::GetInstance().FilesystemMutex()); const std::string directory = self->WallpaperDirectory(); if (FILE* existing = fopen((directory + "/" + to).c_str(), "rb")) { fclose(existing); self->SendError(req, 409, "A file with this name already exists."); return ESP_OK; } if (rename((directory + "/" + from).c_str(), (directory + "/" + to).c_str()) != 0) { self->SendError(req, 404, "File was not found."); return ESP_OK; } } self->refresh_wallpapers_(); self->SendJson(req, 200, "{\"ok\":true}"); return ESP_OK; }
