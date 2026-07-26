/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "local_dashboard_service.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "dht11_service.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "mq2_service.h"
#include "onenet_cloud_service.h"
#include "parent_call_alert_service.h"
#include "sdkconfig.h"

#define LOCAL_DASHBOARD_HTTP_PORT             (80)
#define LOCAL_DASHBOARD_MAX_PHOTOS            (50U)
#define LOCAL_DASHBOARD_PHOTO_NAME_MAX_LEN    (96U)
#define LOCAL_DASHBOARD_PHOTO_PATH_MAX_LEN    (160U)
#define LOCAL_DASHBOARD_FILE_CHUNK_SIZE       (1024U)
#define LOCAL_DASHBOARD_JSON_CHUNK_SIZE       (384U)
#define LOCAL_DASHBOARD_REQUEST_TIMEOUT_SEC   (15U)

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
#define LOCAL_DASHBOARD_PHOTO_DIR CONFIG_BSP_SD_MOUNT_POINT "/camera"
#else
#define LOCAL_DASHBOARD_PHOTO_DIR ""
#endif

static const char *TAG = "LocalDashboard";

typedef struct
{
    char name[LOCAL_DASHBOARD_PHOTO_NAME_MAX_LEN];
    long size;
    time_t mtime;
} local_dashboard_photo_t;

static httpd_handle_t s_httpd;

static const char LOCAL_DASHBOARD_INDEX_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>云边协同影像安全终端</title>"
    "<style>"
    ":root{color-scheme:dark;--bg:#111827;--panel:#182235;--muted:#91a1ba;"
    "--text:#edf2ff;--line:#26344d;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;"
    "--accent:#38bdf8}*{box-sizing:border-box}body{margin:0;background:var(--bg);"
    "color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    "header{position:sticky;top:0;z-index:2;background:rgba(17,24,39,.92);"
    "backdrop-filter:blur(12px);border-bottom:1px solid var(--line);padding:16px}"
    "h1{margin:0;font-size:22px}main{max-width:1180px;margin:0 auto;padding:18px}"
    ".bar{display:flex;gap:10px;align-items:center;justify-content:space-between;flex-wrap:wrap}"
    ".pill{border:1px solid var(--line);border-radius:999px;padding:6px 10px;color:var(--muted)}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}"
    ".card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px}"
    ".cloud-panel{background:linear-gradient(135deg,#102033,#15283e);border:1px solid var(--line);"
    "border-radius:8px;padding:16px}.cloud-title{display:flex;align-items:center;justify-content:space-between;"
    "gap:12px;flex-wrap:wrap}.cloud-flow{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));"
    "gap:10px;margin-top:14px}.cloud-step{border:1px solid var(--line);border-radius:8px;padding:10px;"
    "background:rgba(255,255,255,.03)}.cloud-step b{display:block}.cloud-step span{display:block;"
    "color:var(--muted);font-size:12px;margin-top:5px}"
    ".label{color:var(--muted);font-size:13px}.value{font-size:24px;margin-top:8px;font-weight:700}"
    ".ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}"
    "section{margin-top:18px}button{border:0;border-radius:8px;background:var(--accent);"
    "color:#06121f;padding:9px 12px;font-weight:700}#gallery{display:grid;"
    "grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}.photo{display:block;"
    "background:var(--panel);border:1px solid var(--line);border-radius:8px;overflow:hidden;"
    "color:var(--text);text-decoration:none}.photo img,.empty-thumb{width:100%;aspect-ratio:4/3;"
    "object-fit:cover;background:#0b1220;display:flex;align-items:center;justify-content:center;"
    "color:var(--muted)}.photo div{padding:9px}.photo b{display:block;overflow:hidden;"
    "text-overflow:ellipsis;white-space:nowrap;font-size:13px}.photo small{color:var(--muted)}"
    "@media(max-width:520px){main{padding:12px}.value{font-size:20px}}"
    "</style></head><body><header><div class=\"bar\"><h1>云边协同影像安全终端</h1>"
    "<span class=\"pill\" id=\"ip\">IP: --</span></div></header><main>"
    "<div class=\"grid\" id=\"cards\"></div><section class=\"cloud-panel\" id=\"cloudPanel\"></section>"
    "<section><div class=\"bar\"><h2>SD 卡相册</h2>"
    "<button onclick=\"refreshAll()\">刷新</button></div><div id=\"gallery\"></div></section>"
    "</main><script>"
    "const cards=document.getElementById('cards'),gallery=document.getElementById('gallery'),"
    "cloudPanel=document.getElementById('cloudPanel'),ipEl=document.getElementById('ip');"
    "function esc(v){return String(v==null?'':v).replace(/[&<>\"']/g,m=>({'&':'&amp;','<':'&lt;',"
    "'>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[m]));}"
    "function fmtSize(n){if(!Number.isFinite(n))return'--';if(n>1048576)return(n/1048576).toFixed(1)+' MB';"
    "if(n>1024)return(n/1024).toFixed(1)+' KB';return n+' B';}"
    "function card(label,value,cls=''){return `<article class=\"card\"><div class=\"label\">${label}</div>"
    "<div class=\"value ${cls}\">${value}</div></article>`;}"
    "function renderCloud(c,st){const uuid=c.latest_photo_uuid?esc(c.latest_photo_uuid):'--';"
    "cloudPanel.innerHTML=`<div class=\"cloud-title\"><div><h2>OneNET 云端</h2>"
    "<div class=\"label\">${esc(c.provider)} · ${esc(c.device_id)}</div></div>"
    "<span class=\"pill\">${esc(c.sync_status)}</span></div><div class=\"cloud-flow\">"
    "<div class=\"cloud-step\"><b>边缘采集</b><span>传感器与相机本地实时采集</span></div>"
    "<div class=\"cloud-step\"><b>本地缓存</b><span>${st.photo_count} 张照片</span></div>"
    "<div class=\"cloud-step\"><b>云端通道</b><span>${c.ready?'已接入 OneNET':esc(c.sync_status)}</span></div>"
    "<div class=\"cloud-step\"><b>最新照片</b><span>${esc(c.latest_photo_name||'--')} / ${uuid}</span></div></div>`;}"
    "async function loadStatus(){const r=await fetch('/api/status',{cache:'no-store'});"
    "const s=await r.json();ipEl.textContent='IP: '+(s.network.ip||'no_ip');"
    "const d=s.sensors.dht11,m=s.sensors.mq2,st=s.storage,c=s.cloud;"
    "renderCloud(c,st);"
    "cards.innerHTML=[card('Wi-Fi',s.network.connected?'已获取 IP':'未获取 IP',s.network.connected?'ok':'warn'),"
    "card('温度',d.valid?d.temperature_c.toFixed(1)+' °C':'--',d.valid?'ok':'warn'),"
    "card('湿度',d.valid?d.humidity_percent.toFixed(1)+' %':'--',d.valid?'ok':'warn'),"
    "card('MQ-2',m.enabled?(m.state+(m.alarm?' / ALARM':'')):'disabled',m.alarm?'bad':(m.valid?'ok':'warn')),"
    "card('照片数量',st.sd_enabled?String(st.photo_count):'SD disabled',st.sd_enabled?'ok':'warn'),"
    "card('OneNET',c.ready?'已接入':c.sync_status,c.ready?'ok':'warn')].join('');}"
    "async function loadPhotos(){const r=await fetch('/api/photos',{cache:'no-store'});"
    "const data=await r.json();if(!data.photos||data.photos.length===0){gallery.innerHTML="
    "'<div class=\"card\">暂无照片，先在 Camera App 里拍一张。</div>';return;}"
    "gallery.innerHTML=data.photos.map((p,i)=>{const u='/photos/'+encodeURIComponent(p.name);"
    "const thumb=i<12?`<img loading=\"lazy\" src=\"${u}\" alt=\"${esc(p.name)}\">`:"
    "`<span class=\"empty-thumb\">点击查看</span>`;return `<a class=\"photo\" href=\"${u}\" target=\"_blank\">"
    "${thumb}<div><b>${esc(p.name)}</b><small>${fmtSize(p.size)}</small></div></a>`}).join('');}"
    "async function refreshAll(){try{await Promise.all([loadStatus(),loadPhotos()]);}"
    "catch(e){cards.innerHTML='<article class=\"card\"><div class=\"value bad\">连接失败</div></article>';}}"
    "refreshAll();setInterval(loadStatus,3000);setInterval(loadPhotos,10000);"
    "</script></body></html>";

static esp_err_t local_dashboard_send_chunkf(httpd_req_t *req, const char *fmt, ...)
{
    char buffer[LOCAL_DASHBOARD_JSON_CHUNK_SIZE];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len < 0 || (size_t)len >= sizeof(buffer))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return httpd_resp_send_chunk(req, buffer, len);
}

static esp_err_t local_dashboard_send_json_string(httpd_req_t *req, const char *text)
{
    esp_err_t err = httpd_resp_sendstr_chunk(req, "\"");
    if (err != ESP_OK)
    {
        return err;
    }

    if (text != NULL)
    {
        const unsigned char *cursor = (const unsigned char *)text;
        while (*cursor != '\0')
        {
            char escaped[8];
            switch (*cursor)
            {
            case '\\':
            case '"':
                escaped[0] = '\\';
                escaped[1] = (char)*cursor;
                escaped[2] = '\0';
                err = httpd_resp_sendstr_chunk(req, escaped);
                break;
            case '\n':
                err = httpd_resp_sendstr_chunk(req, "\\n");
                break;
            case '\r':
                err = httpd_resp_sendstr_chunk(req, "\\r");
                break;
            case '\t':
                err = httpd_resp_sendstr_chunk(req, "\\t");
                break;
            default:
                escaped[0] = (char)*cursor;
                escaped[1] = '\0';
                err = httpd_resp_sendstr_chunk(req, escaped);
                break;
            }

            if (err != ESP_OK)
            {
                return err;
            }
            cursor++;
        }
    }

    return httpd_resp_sendstr_chunk(req, "\"");
}

static bool local_dashboard_get_ip(char *ip, size_t ip_len)
{
    if (ip == NULL || ip_len == 0)
    {
        return false;
    }

    ip[0] = '\0';

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL)
    {
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    esp_err_t err = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0)
    {
        return false;
    }

    snprintf(ip, ip_len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

static const char *local_dashboard_dht11_status_name(dht11_service_status_t status)
{
    switch (status)
    {
    case DHT11_SERVICE_STATUS_NEVER_READ:
        return "never_read";
    case DHT11_SERVICE_STATUS_OK:
        return "ok";
    case DHT11_SERVICE_STATUS_TIMEOUT:
        return "timeout";
    case DHT11_SERVICE_STATUS_CHECKSUM_ERROR:
        return "checksum_error";
    case DHT11_SERVICE_STATUS_IO_ERROR:
        return "io_error";
    default:
        return "unknown";
    }
}

static const char *local_dashboard_mq2_state_name(mq2_service_state_t state)
{
    switch (state)
    {
    case MQ2_SERVICE_STATE_WARMING_UP:
        return "warming_up";
    case MQ2_SERVICE_STATE_NORMAL:
        return "normal";
    case MQ2_SERVICE_STATE_ALARM:
        return "alarm";
    case MQ2_SERVICE_STATE_SENSOR_ERROR:
        return "sensor_error";
    default:
        return "unknown";
    }
}

static bool local_dashboard_has_allowed_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
    {
        return false;
    }

    char ext[8] = {};
    size_t ext_len = strnlen(dot, sizeof(ext));
    if (ext_len == 0 || ext_len >= sizeof(ext))
    {
        return false;
    }

    for (size_t i = 0; i < ext_len; ++i)
    {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }

    return strcmp(ext, ".bmp") == 0 || strcmp(ext, ".jpg") == 0 ||
           strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".png") == 0;
}

static bool local_dashboard_is_allowed_photo_name(const char *name)
{
    if (name == NULL)
    {
        return false;
    }

    size_t len = strnlen(name, LOCAL_DASHBOARD_PHOTO_NAME_MAX_LEN);
    if (len == 0 || len >= LOCAL_DASHBOARD_PHOTO_NAME_MAX_LEN)
    {
        return false;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strstr(name, "..") != NULL)
    {
        return false;
    }

    for (size_t i = 0; i < len; ++i)
    {
        const unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.'))
        {
            return false;
        }
    }

    return local_dashboard_has_allowed_extension(name);
}

static esp_err_t local_dashboard_build_photo_path(const char *name,
                                                  char *path,
                                                  size_t path_len)
{
#if !CONFIG_EXAMPLE_ENABLE_SD_CARD
    (void)name;
    (void)path;
    (void)path_len;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!local_dashboard_is_allowed_photo_name(name) || path == NULL || path_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(path, path_len, "%s/%s", LOCAL_DASHBOARD_PHOTO_DIR, name);
    if (written < 0 || (size_t)written >= path_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
#endif
}

static bool local_dashboard_photo_is_newer(const local_dashboard_photo_t *candidate,
                                           const local_dashboard_photo_t *current)
{
    if (candidate->mtime != current->mtime)
    {
        return candidate->mtime > current->mtime;
    }

    return strcmp(candidate->name, current->name) > 0;
}

static void local_dashboard_insert_photo(local_dashboard_photo_t *photos,
                                         size_t max_photos,
                                         size_t *photo_count,
                                         const local_dashboard_photo_t *candidate)
{
    size_t insert_at = 0;
    while (insert_at < *photo_count &&
           !local_dashboard_photo_is_newer(candidate, &photos[insert_at]))
    {
        insert_at++;
    }

    if (insert_at >= max_photos)
    {
        return;
    }

    size_t limit = *photo_count;
    if (limit >= max_photos)
    {
        limit = max_photos - 1;
    }
    else
    {
        (*photo_count)++;
    }

    for (size_t i = limit; i > insert_at; --i)
    {
        photos[i] = photos[i - 1];
    }
    photos[insert_at] = *candidate;
}

static esp_err_t local_dashboard_scan_photos(local_dashboard_photo_t *photos,
                                             size_t max_photos,
                                             size_t *visible_count,
                                             size_t *total_count)
{
    if (visible_count == NULL || total_count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *visible_count = 0;
    *total_count = 0;

#if !CONFIG_EXAMPLE_ENABLE_SD_CARD
    (void)photos;
    (void)max_photos;
    return ESP_ERR_NOT_SUPPORTED;
#else
    DIR *dir = opendir(LOCAL_DASHBOARD_PHOTO_DIR);
    if (dir == NULL)
    {
        if (errno == ENOENT)
        {
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Failed to open photo directory %s: errno=%d",
                 LOCAL_DASHBOARD_PHOTO_DIR, errno);
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!local_dashboard_is_allowed_photo_name(entry->d_name))
        {
            continue;
        }

        char path[LOCAL_DASHBOARD_PHOTO_PATH_MAX_LEN];
        esp_err_t err = local_dashboard_build_photo_path(entry->d_name, path, sizeof(path));
        if (err != ESP_OK)
        {
            continue;
        }

        struct stat st = {};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }

        (*total_count)++;
        if (photos == NULL || max_photos == 0)
        {
            continue;
        }

        local_dashboard_photo_t photo = {};
        snprintf(photo.name, sizeof(photo.name), "%s", entry->d_name);
        photo.size = (long)st.st_size;
        photo.mtime = st.st_mtime;
        local_dashboard_insert_photo(photos, max_photos, visible_count, &photo);
    }

    closedir(dir);
    return ESP_OK;
#endif
}

static const char *local_dashboard_content_type_for_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
    {
        return "application/octet-stream";
    }

    char ext[8] = {};
    size_t ext_len = strnlen(dot, sizeof(ext));
    if (ext_len == 0 || ext_len >= sizeof(ext))
    {
        return "application/octet-stream";
    }

    for (size_t i = 0; i < ext_len; ++i)
    {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }

    if (strcmp(ext, ".bmp") == 0)
    {
        return "image/bmp";
    }
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
    {
        return "image/jpeg";
    }
    if (strcmp(ext, ".png") == 0)
    {
        return "image/png";
    }

    return "application/octet-stream";
}

static esp_err_t local_dashboard_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, LOCAL_DASHBOARD_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t local_dashboard_status_handler(httpd_req_t *req)
{
    char ip[16] = {};
    bool has_ip = local_dashboard_get_ip(ip, sizeof(ip));
    size_t photo_count = 0;
    size_t visible_count = 0;
    (void)local_dashboard_scan_photos(NULL, 0, &visible_count, &photo_count);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(req,
                                                    "{\"uptime_ms\":%lld,",
                                                    (long long)(esp_timer_get_time() / 1000)),
                        TAG, "Failed to send status JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(req,
                                                    "\"network\":{\"connected\":%s,\"ip\":",
                                                    has_ip ? "true" : "false"),
                        TAG, "Failed to send network JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(req, has_ip ? ip : ""),
                        TAG, "Failed to send IP JSON");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "},\"sensors\":{"),
                        TAG, "Failed to send sensor JSON");

#if CONFIG_EXAMPLE_ENABLE_DHT11_SERVICE
    dht11_service_snapshot_t dht_snapshot = {};
    esp_err_t dht_err = dht11_service_get_snapshot(&dht_snapshot);
    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            "\"dht11\":{\"enabled\":true,\"valid\":%s,"
                            "\"status\":\"%s\",\"last_error\":%d,"
                            "\"temperature_c\":%.1f,\"humidity_percent\":%.1f,"
                            "\"sample_seq\":%lu,\"timestamp_ms\":%lld},",
                            (dht_err == ESP_OK && dht_snapshot.valid &&
                             dht_snapshot.status == DHT11_SERVICE_STATUS_OK)
                                ? "true"
                                : "false",
                            dht_err == ESP_OK ? local_dashboard_dht11_status_name(dht_snapshot.status)
                                              : "unavailable",
                            dht_err == ESP_OK ? dht_snapshot.last_error : dht_err,
                            dht_err == ESP_OK ? (double)dht_snapshot.temperature_c : 0.0,
                            dht_err == ESP_OK ? (double)dht_snapshot.humidity_percent : 0.0,
                            dht_err == ESP_OK ? (unsigned long)dht_snapshot.sample_seq : 0UL,
                            dht_err == ESP_OK ? (long long)dht_snapshot.timestamp_ms : 0LL),
                        TAG, "Failed to send DHT11 JSON");
#else
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(
                            req,
                            "\"dht11\":{\"enabled\":false,\"valid\":false,"
                            "\"status\":\"disabled\",\"last_error\":0,"
                            "\"temperature_c\":0,\"humidity_percent\":0,"
                            "\"sample_seq\":0,\"timestamp_ms\":0},"),
                        TAG, "Failed to send disabled DHT11 JSON");
#endif

#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE
    mq2_service_snapshot_t mq2_snapshot = {};
    esp_err_t mq2_err = mq2_service_get_snapshot(&mq2_snapshot);
    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            "\"mq2\":{\"enabled\":true,\"valid\":%s,"
                            "\"state\":\"%s\",\"last_error\":%d,"
                            "\"digital_level\":%d,\"alarm\":%s,"
                            "\"stable_count\":%lu,\"sample_seq\":%lu,"
                            "\"timestamp_ms\":%lld}",
                            (mq2_err == ESP_OK && mq2_snapshot.valid) ? "true" : "false",
                            mq2_err == ESP_OK ? local_dashboard_mq2_state_name(mq2_snapshot.state)
                                              : "unavailable",
                            mq2_err == ESP_OK ? mq2_snapshot.last_error : mq2_err,
                            mq2_err == ESP_OK ? mq2_snapshot.digital_level : -1,
                            (mq2_err == ESP_OK && mq2_snapshot.alarm_asserted) ? "true" : "false",
                            mq2_err == ESP_OK ? (unsigned long)mq2_snapshot.stable_count : 0UL,
                            mq2_err == ESP_OK ? (unsigned long)mq2_snapshot.sample_seq : 0UL,
                            mq2_err == ESP_OK ? (long long)mq2_snapshot.timestamp_ms : 0LL),
                        TAG, "Failed to send MQ-2 JSON");
#else
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(
                            req,
                            "\"mq2\":{\"enabled\":false,\"valid\":false,"
                            "\"state\":\"disabled\",\"last_error\":0,"
                            "\"digital_level\":-1,\"alarm\":false,"
                            "\"stable_count\":0,\"sample_seq\":0,\"timestamp_ms\":0}"),
                        TAG, "Failed to send disabled MQ-2 JSON");
#endif

    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "},"),
                        TAG, "Failed to close sensors JSON");

#if CONFIG_EXAMPLE_ENABLE_PARENT_CALL_ALERT_SERVICE
    parent_call_alert_snapshot_t alert_snapshot = {};
    esp_err_t alert_err = parent_call_alert_service_get_snapshot(&alert_snapshot);
    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            "\"alert\":{\"enabled\":%s,\"status\":\"%s\","
                            "\"last_error\":%d,\"last_http_status\":%d,"
                            "\"queued_count\":%lu,\"sent_count\":%lu,"
                            "\"skipped_count\":%lu,\"failed_count\":%lu,"
                            "\"modem_ready\":%s,\"modem_busy\":%s,"
                            "\"last_reason\":",
                            alert_err == ESP_OK && alert_snapshot.initialized ? "true" : "false",
                            alert_err == ESP_OK
                                ? parent_call_alert_service_status_name(alert_snapshot.status)
                                : "unavailable",
                            alert_err == ESP_OK ? alert_snapshot.last_error : alert_err,
                            alert_err == ESP_OK ? alert_snapshot.last_http_status : 0,
                            alert_err == ESP_OK ? (unsigned long)alert_snapshot.queued_count : 0UL,
                            alert_err == ESP_OK ? (unsigned long)alert_snapshot.sent_count : 0UL,
                            alert_err == ESP_OK ? (unsigned long)alert_snapshot.skipped_count : 0UL,
                            alert_err == ESP_OK ? (unsigned long)alert_snapshot.failed_count : 0UL,
                            (alert_err == ESP_OK && alert_snapshot.modem_ready) ? "true" : "false",
                            (alert_err == ESP_OK && alert_snapshot.modem_busy) ? "true" : "false"),
                        TAG, "Failed to send alert JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(
                            req, alert_err == ESP_OK ? alert_snapshot.last_reason : ""),
                        TAG, "Failed to send alert reason JSON");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "},"),
                        TAG, "Failed to close alert JSON");
#else
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(
                            req,
                            "\"alert\":{\"enabled\":false,\"status\":\"disabled\","
                            "\"last_error\":0,\"last_http_status\":0,"
                            "\"queued_count\":0,\"sent_count\":0,\"skipped_count\":0,"
                            "\"failed_count\":0,\"modem_ready\":false,\"modem_busy\":false,"
                            "\"last_reason\":\"\"},"),
                        TAG, "Failed to send disabled alert JSON");
#endif

    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            "\"storage\":{\"sd_enabled\":%s,\"photo_dir\":",
#if CONFIG_EXAMPLE_ENABLE_SD_CARD
                            "true"
#else
                            "false"
#endif
                            ),
                        TAG, "Failed to send storage JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(req, LOCAL_DASHBOARD_PHOTO_DIR),
                        TAG, "Failed to send photo dir JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            ",\"photo_count\":%lu,\"photo_limit\":%u},\"cloud\":{",
                            (unsigned long)photo_count,
                            (unsigned int)LOCAL_DASHBOARD_MAX_PHOTOS),
                        TAG, "Failed to send storage count JSON");

    onenet_cloud_snapshot_t cloud_snapshot = {};
    esp_err_t cloud_err = onenet_cloud_service_get_snapshot(&cloud_snapshot);
    const bool cloud_ready = cloud_err == ESP_OK &&
                             (cloud_snapshot.mqtt_connected ||
                              cloud_snapshot.status == ONENET_CLOUD_STATUS_PUBLISHED ||
                              cloud_snapshot.status == ONENET_CLOUD_STATUS_UPLOAD_OK);
    const char *cloud_mode = cloud_snapshot.enabled ? "onenet" : "disabled";
    const char *cloud_status_name =
        onenet_cloud_service_status_name(cloud_err == ESP_OK ? cloud_snapshot.status
                                                             : ONENET_CLOUD_STATUS_INTERNAL_ERROR);
    const char *cloud_status_text =
        onenet_cloud_service_status_text(cloud_err == ESP_OK ? cloud_snapshot.status
                                                             : ONENET_CLOUD_STATUS_INTERNAL_ERROR);

    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            "\"ready\":%s,\"mode\":\"%s\",\"provider\":\"OneNET Studio\","
                            "\"configured\":%s,\"mqtt_connected\":%s,"
                            "\"status\":\"%s\",\"sync_status\":",
                            cloud_ready ? "true" : "false",
                            cloud_mode,
                            (cloud_err == ESP_OK && cloud_snapshot.configured) ? "true" : "false",
                            (cloud_err == ESP_OK && cloud_snapshot.mqtt_connected) ? "true" : "false",
                            cloud_status_name),
                        TAG, "Failed to send cloud status JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(req, cloud_status_text),
                        TAG, "Failed to send cloud status text JSON");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, ",\"product_id\":"),
                        TAG, "Failed to send cloud product key");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(
                            req, cloud_err == ESP_OK ? cloud_snapshot.product_id : ""),
                        TAG, "Failed to send cloud product JSON");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, ",\"device_id\":"),
                        TAG, "Failed to send cloud device key");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(
                            req, (cloud_err == ESP_OK && cloud_snapshot.device_name[0] != '\0')
                                     ? cloud_snapshot.device_name
                                     : "esp32p4-onenet"),
                        TAG, "Failed to send cloud device JSON");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, ",\"latest_photo_name\":"),
                        TAG, "Failed to send cloud photo name key");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(
                            req, cloud_err == ESP_OK ? cloud_snapshot.last_photo_name : ""),
                        TAG, "Failed to send cloud photo name JSON");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, ",\"latest_photo_uuid\":"),
                        TAG, "Failed to send cloud uuid key");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(
                            req, cloud_err == ESP_OK ? cloud_snapshot.last_photo_uuid : ""),
                        TAG, "Failed to send cloud uuid JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            ",\"upload_queue\":%lu,\"publish_count\":%lu,"
                            "\"failed_publish_count\":%lu,\"photo_upload_count\":%lu,"
                            "\"failed_photo_upload_count\":%lu,\"last_sync_ms\":%lld,"
                            "\"last_photo_upload_ms\":%lld,\"last_http_status\":%d}}",
                            (unsigned long)photo_count,
                            cloud_err == ESP_OK ? (unsigned long)cloud_snapshot.publish_count : 0UL,
                            cloud_err == ESP_OK ? (unsigned long)cloud_snapshot.failed_publish_count : 0UL,
                            cloud_err == ESP_OK ? (unsigned long)cloud_snapshot.photo_upload_count : 0UL,
                            cloud_err == ESP_OK ? (unsigned long)cloud_snapshot.failed_photo_upload_count : 0UL,
                            cloud_err == ESP_OK ? (long long)cloud_snapshot.last_publish_ms : 0LL,
                            cloud_err == ESP_OK ? (long long)cloud_snapshot.last_photo_upload_ms : 0LL,
                            cloud_err == ESP_OK ? cloud_snapshot.last_http_status : 0),
                        TAG, "Failed to send final cloud JSON");

    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t local_dashboard_photos_handler(httpd_req_t *req)
{
    local_dashboard_photo_t photos[LOCAL_DASHBOARD_MAX_PHOTOS] = {};
    size_t visible_count = 0;
    size_t total_count = 0;
    esp_err_t scan_err = local_dashboard_scan_photos(photos, LOCAL_DASHBOARD_MAX_PHOTOS,
                                                     &visible_count, &total_count);
    if (scan_err != ESP_OK && scan_err != ESP_ERR_NOT_SUPPORTED)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to scan photo directory");
        return scan_err;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            "{\"sd_enabled\":%s,\"dir\":",
#if CONFIG_EXAMPLE_ENABLE_SD_CARD
                            "true"
#else
                            "false"
#endif
                            ),
                        TAG, "Failed to send photos header JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(req, LOCAL_DASHBOARD_PHOTO_DIR),
                        TAG, "Failed to send photos dir JSON");
    ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                            req,
                            ",\"total\":%lu,\"limit\":%u,\"photos\":[",
                            (unsigned long)total_count,
                            (unsigned int)LOCAL_DASHBOARD_MAX_PHOTOS),
                        TAG, "Failed to send photos metadata JSON");

    for (size_t i = 0; i < visible_count; ++i)
    {
        if (i > 0)
        {
            ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, ","),
                                TAG, "Failed to send photo separator");
        }

        ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "{\"name\":"),
                            TAG, "Failed to send photo object");
        ESP_RETURN_ON_ERROR(local_dashboard_send_json_string(req, photos[i].name),
                            TAG, "Failed to send photo name");
        ESP_RETURN_ON_ERROR(local_dashboard_send_chunkf(
                                req,
                                ",\"size\":%ld,\"mtime\":%lld}",
                                photos[i].size,
                                (long long)photos[i].mtime),
                            TAG, "Failed to send photo metadata");
    }

    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, "]}"),
                        TAG, "Failed to close photos JSON");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t local_dashboard_photo_file_handler(httpd_req_t *req)
{
#if !CONFIG_EXAMPLE_ENABLE_SD_CARD
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "SD card disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    const char *prefix = "/photos/";
    const size_t prefix_len = strlen(prefix);
    if (strncmp(req->uri, prefix, prefix_len) != 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid photo URI");
        return ESP_ERR_INVALID_ARG;
    }

    char name[LOCAL_DASHBOARD_PHOTO_NAME_MAX_LEN] = {};
    const char *uri_name = req->uri + prefix_len;
    size_t name_len = 0;
    while (uri_name[name_len] != '\0' && uri_name[name_len] != '?' &&
           name_len < sizeof(name) - 1)
    {
        name[name_len] = uri_name[name_len];
        name_len++;
    }
    name[name_len] = '\0';

    char path[LOCAL_DASHBOARD_PHOTO_PATH_MAX_LEN];
    esp_err_t err = local_dashboard_build_photo_path(name, path, sizeof(path));
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid photo name");
        return err;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Photo not found");
        return ESP_ERR_NOT_FOUND;
    }

    httpd_resp_set_type(req, local_dashboard_content_type_for_name(name));
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=30");

    char chunk[LOCAL_DASHBOARD_FILE_CHUNK_SIZE];
    size_t read_len = 0;
    while ((read_len = fread(chunk, 1, sizeof(chunk), file)) > 0)
    {
        err = httpd_resp_send_chunk(req, chunk, read_len);
        if (err != ESP_OK)
        {
            fclose(file);
            return err;
        }
    }

    if (ferror(file))
    {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read photo");
        return ESP_FAIL;
    }

    fclose(file);
    return httpd_resp_sendstr_chunk(req, NULL);
#endif
}

static esp_err_t local_dashboard_prepare_network_stack(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    return ESP_OK;
}

esp_err_t local_dashboard_service_start(void)
{
    if (s_httpd != NULL)
    {
        return ESP_OK;
    }

    esp_err_t err = local_dashboard_prepare_network_stack();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to prepare network stack: %s", esp_err_to_name(err));
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = LOCAL_DASHBOARD_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = LOCAL_DASHBOARD_REQUEST_TIMEOUT_SEC;
    config.send_wait_timeout = LOCAL_DASHBOARD_REQUEST_TIMEOUT_SEC;
    config.stack_size = 8192;

    err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start local dashboard HTTP server: %s",
                 esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = local_dashboard_index_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = local_dashboard_status_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t photos_uri = {
        .uri = "/api/photos",
        .method = HTTP_GET,
        .handler = local_dashboard_photos_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t photo_file_uri = {
        .uri = "/photos/*",
        .method = HTTP_GET,
        .handler = local_dashboard_photo_file_handler,
        .user_ctx = NULL,
    };

    err = httpd_register_uri_handler(s_httpd, &index_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register index handler: %s", esp_err_to_name(err));
        goto err_stop;
    }

    err = httpd_register_uri_handler(s_httpd, &status_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register status handler: %s", esp_err_to_name(err));
        goto err_stop;
    }

    err = httpd_register_uri_handler(s_httpd, &photos_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register photos handler: %s", esp_err_to_name(err));
        goto err_stop;
    }

    err = httpd_register_uri_handler(s_httpd, &photo_file_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register photo file handler: %s", esp_err_to_name(err));
        goto err_stop;
    }

    ESP_LOGI(TAG, "Local dashboard started on http://<device-ip>/");
    return ESP_OK;

err_stop:
    httpd_stop(s_httpd);
    s_httpd = NULL;
    return err;
}

esp_err_t local_dashboard_service_stop(void)
{
    if (s_httpd == NULL)
    {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_httpd);
    s_httpd = NULL;
    return err;
}
