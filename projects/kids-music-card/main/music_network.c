#include "music_network.h"
#include "music_policy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define NET_IP BIT0
#define NET_TIME BIT1
#define PORTAL_MS 300000
static const char *TAG = "music_network";
static EventGroupHandle_t s_events;
static QueueHandle_t s_commands;
static SemaphoreHandle_t s_info_lock;
static esp_netif_t *s_sta, *s_ap;
static httpd_handle_t s_server;
static atomic_int s_state;
static atomic_bool s_saved;
static bool s_initialized, s_sntp_started;
static char s_ap_name[33], s_ap_password[9], s_token[33];
typedef enum { NET_CONNECT, NET_PROVISION, NET_CANCEL, NET_SCAN, NET_SAVE_WIFI, NET_SAVE_AUTH } command_t;
static char s_scan_names[12][33];
static int s_scan_count;
static atomic_int s_save_status, s_disconnect_reason;
static bool s_edit_pending;
static char s_edit_name[65], s_edit_password[129];

static void clear(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size--) *bytes++ = 0;
}

static void time_synced(struct timeval *value)
{
    (void)value;
    if (s_events) xEventGroupSetBits(s_events, NET_TIME);
    ESP_LOGI(TAG, "SNTP time synchronized");
}

static void network_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, NET_IP);
        ESP_LOGI(TAG, "Wi-Fi obtained IP; starting clock synchronization");
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, NET_IP);
        if (data) atomic_store(&s_disconnect_reason, ((wifi_event_sta_disconnected_t *)data)->reason);
    }
}

static esp_err_t initialize(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    bool driver = false, wifi_handler = false, ip_handler = false;
    const esp_netif_config_t sta_config = ESP_NETIF_DEFAULT_WIFI_STA();
    const esp_netif_config_t ap_config = ESP_NETIF_DEFAULT_WIFI_AP();
    s_sta = esp_netif_new(&sta_config); s_ap = esp_netif_new(&ap_config);
    if (!s_sta || !s_ap) { error = ESP_ERR_NO_MEM; goto fail; }
    if ((error = esp_netif_attach_wifi_station(s_sta)) != ESP_OK ||
        (error = esp_netif_attach_wifi_ap(s_ap)) != ESP_OK ||
        (error = esp_wifi_set_default_wifi_sta_handlers()) != ESP_OK ||
        (error = esp_wifi_set_default_wifi_ap_handlers()) != ESP_OK) goto fail;
    const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if ((error = esp_wifi_init(&config)) != ESP_OK) goto fail;
    driver = true;
    if ((error = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, network_event, NULL)) != ESP_OK) goto fail;
    wifi_handler = true;
    if ((error = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL)) != ESP_OK) goto fail;
    ip_handler = true;
    if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) goto fail;
    /* Fill the server list within the compiled limit. CONFIG_LWIP_SNTP_MAX_SERVERS
     * can be smaller than the candidate list; the runtime count must match the
     * array size or the SNTP init is rejected. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(1, ESP_SNTP_SERVER_LIST("ntp.aliyun.com"));
    const char *servers[] = {"ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org"};
    sntp.num_of_servers = 0;
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]) && i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i)
        sntp.servers[sntp.num_of_servers++] = servers[i];
    sntp.start = false; sntp.sync_cb = time_synced;
    if ((error = esp_netif_sntp_init(&sntp)) != ESP_OK) goto fail;
    s_initialized = true;
    return ESP_OK;
fail:
    if (ip_handler) esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event);
    if (wifi_handler) esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, network_event);
    if (driver) esp_wifi_deinit();
    if (s_sta) { esp_netif_destroy_default_wifi(s_sta); s_sta = NULL; }
    if (s_ap) { esp_netif_destroy_default_wifi(s_ap); s_ap = NULL; }
    return error;
}

static bool valid_password(const char *value)
{
    size_t size = strlen(value);
    return size == 0 || (size >= 8 && size <= 63);
}

static esp_err_t connect_saved(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("music_net", NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    char ssid[33] = {0}, password[64] = {0};
    size_t length = sizeof(ssid);
    error = nvs_get_str(handle, "ssid", ssid, &length);
    length = sizeof(password);
    if (error == ESP_OK) error = nvs_get_str(handle, "wifi_pass", password, &length);
    nvs_close(handle);
    if (error == ESP_OK && (!ssid[0] || !valid_password(password))) error = ESP_ERR_INVALID_ARG;
    if (error != ESP_OK) { clear(password, sizeof(password)); return error; }
    if ((error = initialize()) != ESP_OK) { clear(password, sizeof(password)); return error; }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    clear(password, sizeof(password));
    s_sntp_started = false; /* Restart SNTP after a manual reconnect/new network. */
    (void)esp_wifi_disconnect();
    xEventGroupClearBits(s_events, NET_IP);
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_STA, &config);
    clear(&config, sizeof(config));
    if (error == ESP_OK) error = esp_wifi_start();
    if (error == ESP_OK) { atomic_store(&s_state, MUSIC_NET_CONNECTING); error = esp_wifi_connect(); }
    return error;
}

static bool local_request(httpd_req_t *request)
{
    char host[64];
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
    return !strcmp(host, "192.168.4.1") || !strcmp(host, "192.168.4.1:80");
}

static esp_err_t portal_get(httpd_req_t *request)
{
    if (!local_request(request) || music_network_state() != MUSIC_NET_PROVISIONING)
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Local setup only");
    /* No external resources; no credentials are prefilled or echoed. */
    const char *format =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<title>KidMusic setup</title><h2>儿童音乐卡配网</h2>"
        "<p>源站: " MUSIC_DEFAULT_ORIGIN "</p><form id=f>"
        "<label>2.4 GHz Wi-Fi <input name=ssid maxlength=32 required></label><br>"
        "<label>Wi-Fi 密码 <input name=wifi_pass type=password maxlength=63 autocomplete=new-password></label><br>"
        "<label>曲库用户名 <input name=auth_user maxlength=64></label><br>"
        "<label>曲库密码 <input name=auth_pass type=password maxlength=128 autocomplete=new-password></label><br>"
        "<button>保存并连接</button></form><p id=result></p>"
        "<script>f.onsubmit=async e=>{e.preventDefault();let d=Object.fromEntries(new FormData(f));"
        "try{let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json',"
        "'X-Setup-Token':'%s'},body:JSON.stringify(d)});"
        "result.textContent=r.ok?'已保存，稍后热点关闭，请返回设备查看状态':'保存失败，请检查输入';"
        "if(r.ok){f.reset();f.querySelector('button').disabled=true}}catch(e){result.textContent='连接失败，请重试'}};</script>";
    char *html = malloc(2048);
    if (!html) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory unavailable");
    snprintf(html, 2048, format, s_token);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'");
    esp_err_t error = httpd_resp_send(request, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return error;
}

static const char *field(cJSON *json, const char *name, size_t maximum)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) > maximum) return NULL;
    int count = 0;
    for (cJSON *item = json->child; item; item = item->next)
        if (item->string && !strcmp(item->string, name)) ++count;
    return count == 1 ? value->valuestring : NULL;
}

static bool auth_string(const char *value, bool user)
{
    for (; *value; ++value) if ((unsigned char)*value < 32 || (unsigned char)*value > 126 || (user && *value == ':')) return false;
    return true;
}

static esp_err_t portal_save(httpd_req_t *request)
{
    char token[40], type[48];
    if (!local_request(request) || music_network_state() != MUSIC_NET_PROVISIONING ||
        httpd_req_get_hdr_value_str(request, "X-Setup-Token", token, sizeof(token)) != ESP_OK || strcmp(token, s_token) ||
        httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK || strcmp(type, "application/json"))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Invalid setup request");
    if (request->content_len <= 0 || request->content_len > 1024)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid input size");
    char body[1025]; int used = 0;
    while (used < request->content_len) {
        int got = httpd_req_recv(request, body + used, request->content_len - used);
        if (got <= 0) { clear(body, sizeof(body)); return ESP_FAIL; }
        used += got;
    }
    body[used] = 0;
    /* Setup JSON must be a flat object; reject nested data before parsing. */
    bool nested = false, quoted = false, escaped = false;
    for (int i = 1; i < used - 1; ++i) {
        char c = body[i];
        if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') nested = true;
    }
    cJSON *json = nested || memchr(body, 0, used) || strstr(body, "\\u0000") ? NULL : cJSON_ParseWithOpts(body, NULL, true);
    const char *ssid = json ? field(json, "ssid", 32) : NULL;
    const char *pass = json ? field(json, "wifi_pass", 63) : NULL;
    const char *user = json ? field(json, "auth_user", 64) : NULL;
    const char *auth = json ? field(json, "auth_pass", 128) : NULL;
    bool valid = ssid && *ssid && pass && valid_password(pass) && user && auth &&
        auth_string(user, true) && auth_string(auth, false) && (!!*user == !!*auth);
    esp_err_t error = ESP_ERR_INVALID_ARG;
    if (valid) {
        nvs_handle_t handle;
        error = nvs_open("music_net", NVS_READWRITE, &handle);
        if (error == ESP_OK) {
            error = nvs_set_str(handle, "ssid", ssid);
            if (error == ESP_OK) error = nvs_set_str(handle, "wifi_pass", pass);
            if (error == ESP_OK) error = nvs_set_str(handle, "origin", MUSIC_DEFAULT_ORIGIN);
            if (error == ESP_OK) error = nvs_set_str(handle, "auth_user", user);
            if (error == ESP_OK) error = nvs_set_str(handle, "auth_pass", auth);
            if (error == ESP_OK) error = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    clear(body, sizeof(body));
    if (json) {
        for (cJSON *item = json->child; item; item = item->next)
            if (item->valuestring) clear(item->valuestring, strlen(item->valuestring));
        cJSON_Delete(json);
    }
    if (error != ESP_OK) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Cannot save configuration");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    error = httpd_resp_sendstr(request, "Saved");
    atomic_store(&s_saved, true);
    return error;
}

static void close_portal(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    atomic_store(&s_saved, false);
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    clear(s_ap_password, sizeof(s_ap_password)); clear(s_token, sizeof(s_token)); s_ap_name[0] = 0;
    xSemaphoreGive(s_info_lock);
}

static esp_err_t open_portal(void)
{
    esp_err_t error = initialize();
    if (error != ESP_OK) return error;
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
    xEventGroupClearBits(s_events, NET_IP);
    wifi_config_t config = {0};
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    snprintf(s_ap_name, sizeof(s_ap_name), "KidMusic-%04X", (unsigned)(esp_random() & 0xffff));
    const char *alphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    for (int i = 0; i < 8; ++i) s_ap_password[i] = alphabet[esp_random() % strlen(alphabet)];
    s_ap_password[8] = 0;
    for (int i = 0; i < 16; ++i) snprintf(s_token + i * 2, 3, "%02x", (unsigned)(esp_random() & 0xff));
    strcpy((char *)config.ap.ssid, s_ap_name);
    strcpy((char *)config.ap.password, s_ap_password);
    xSemaphoreGive(s_info_lock);
    config.ap.ssid_len = strlen((char *)config.ap.ssid);
    config.ap.channel = 1; config.ap.max_connection = 1; config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    error = esp_wifi_set_mode(WIFI_MODE_AP); /* No STA: setup server is not exposed to home LAN. */
    if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_AP, &config);
    clear(&config, sizeof(config));
    if (error == ESP_OK) error = esp_wifi_start();
    if (error != ESP_OK) return error;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_open_sockets = 2; http.max_uri_handlers = 2; http.stack_size = 6144;
    http.recv_wait_timeout = 3; http.send_wait_timeout = 3; http.lru_purge_enable = true;
    error = httpd_start(&s_server, &http);
    if (error != ESP_OK) return error;
    const httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = portal_get};
    const httpd_uri_t post = {.uri = "/save", .method = HTTP_POST, .handler = portal_save};
    if ((error = httpd_register_uri_handler(s_server, &get)) == ESP_OK)
        error = httpd_register_uri_handler(s_server, &post);
    if (error == ESP_OK) atomic_store(&s_state, MUSIC_NET_PROVISIONING);
    return error;
}

static esp_err_t scan_networks(void)
{
    esp_err_t error = initialize();
    if (error != ESP_OK) return error;
    (void)esp_wifi_disconnect();
    xEventGroupClearBits(s_events, NET_IP);
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) error = esp_wifi_start();
    wifi_scan_config_t scan = {.show_hidden = false};
    if (error == ESP_OK) error = esp_wifi_scan_start(&scan, true);
    wifi_ap_record_t *records = calloc(12, sizeof(*records));
    uint16_t count = 12;
    if (!records) { esp_wifi_clear_ap_list(); return ESP_ERR_NO_MEM; }
    if (error == ESP_OK) error = esp_wifi_scan_get_ap_records(&count, records);
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    s_scan_count = error == ESP_OK ? 0 : -2;
    for (int i = 0; error == ESP_OK && i < count; ++i) {
        bool duplicate = false;
        for (int j = 0; j < s_scan_count; ++j)
            if (!strcmp(s_scan_names[j], (const char *)records[i].ssid)) duplicate = true;
        if (!duplicate && records[i].ssid[0])
            snprintf(s_scan_names[s_scan_count++], 33, "%s", records[i].ssid);
    }
    xSemaphoreGive(s_info_lock);
    free(records);
    atomic_store(&s_state, MUSIC_NET_OFFLINE);
    return error;
}

static esp_err_t save_edit(bool wifi)
{
    nvs_handle_t handle;
    char name[65], password[129];
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    memcpy(name, s_edit_name, sizeof(name)); memcpy(password, s_edit_password, sizeof(password));
    xSemaphoreGive(s_info_lock);
    esp_err_t error = nvs_open("music_net", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, wifi ? "ssid" : "auth_user", name);
        if (error == ESP_OK) error = nvs_set_str(handle, wifi ? "wifi_pass" : "auth_pass", password);
        if (error == ESP_OK && !wifi) error = nvs_set_str(handle, "origin", MUSIC_DEFAULT_ORIGIN);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    clear(name, sizeof(name)); clear(password, sizeof(password));
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    clear(s_edit_name, sizeof(s_edit_name)); clear(s_edit_password, sizeof(s_edit_password));
    s_edit_pending = false;
    atomic_store(&s_save_status, error == ESP_OK ? 2 : -1);
    xSemaphoreGive(s_info_lock);
    return error;
}

/* The device must be able to reach the configured catalog host. Some routers
 * or ISP resolvers fail to resolve it (observed on a real network: the origin
 * returned EAI_FAIL for this name while other names resolved normally). Keep
 * the DHCP-provided server, but if the origin host cannot be resolved, switch
 * to a public resolver once and report the outcome. */
static void ensure_origin_dns(void)
{
    char origin[128] = MUSIC_DEFAULT_ORIGIN, host[128];
    nvs_handle_t handle;
    if (nvs_open("music_net", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(origin);
        if (nvs_get_str(handle, "origin", origin, &size) != ESP_OK) strcpy(origin, MUSIC_DEFAULT_ORIGIN);
        nvs_close(handle);
    }
    snprintf(host, sizeof(host), "%s", strncmp(origin, "https://", 8) ? origin : origin + 8);
    char *slash = strchr(host, '/');
    if (slash) *slash = 0;
    if (!host[0]) return;
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *list = NULL;
    if (getaddrinfo(host, "443", &hints, &list) == 0) {
        freeaddrinfo(list);
        return; /* The network-provided resolver works. */
    }
    ESP_LOGW(TAG, "The network resolver cannot resolve %s; trying a public resolver", host);
    /* Only keep a public resolver if it actually resolves this host: some
     * networks block outbound DNS, and blindly switching would break every
     * other lookup on the device. */
    esp_netif_dns_info_t saved;
    const bool have_saved = esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &saved) == ESP_OK;
    const char *candidates[] = {"223.5.5.5", "119.29.29.29"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        esp_netif_dns_info_t dns = {.ip = {.type = ESP_IPADDR_TYPE_V4}};
        if (esp_netif_str_to_ip4(candidates[i], &dns.ip.u_addr.ip4) != ESP_OK ||
            esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) continue;
        for (int attempt = 0; attempt < 2; ++attempt) {
            list = NULL;
            if (getaddrinfo(host, "443", &hints, &list) == 0) {
                char text[64] = "-";
                if (list && list->ai_family == AF_INET)
                    inet_ntop(AF_INET, &((struct sockaddr_in *)list->ai_addr)->sin_addr, text, sizeof(text));
                ESP_LOGI(TAG, "Resolver %s resolved %s to %s", candidates[i], host, text);
                freeaddrinfo(list);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (have_saved) (void)esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &saved);
    ESP_LOGW(TAG, "No resolver could resolve %s; fix this network's DNS (the catalog host still resolves from other networks)", host);
}

static void worker(void *argument)
{
    (void)argument;
    command_t command = NET_CONNECT;
    bool dns_checked = false;
    int64_t portal_until = 0, retry_at = 0, connect_until = 0, time_until = 0;
    unsigned backoff = 1000;
    bool pending = true;
    for (;;) {
        if (!pending) pending = xQueueReceive(s_commands, &command, pdMS_TO_TICKS(250)) == pdTRUE;
        if (pending) {
            pending = false;
            if (s_server) close_portal();
            esp_err_t error;
            if (command == NET_SCAN) {
                error = scan_networks();
                if (error != ESP_OK) {
                    xSemaphoreTake(s_info_lock, portMAX_DELAY); s_scan_count = -2; xSemaphoreGive(s_info_lock);
                    atomic_store(&s_state, MUSIC_NET_ERROR);
                }
                connect_until = time_until = 0;
                continue;
            }
            if (command == NET_SAVE_WIFI || command == NET_SAVE_AUTH) {
                error = save_edit(command == NET_SAVE_WIFI);
                if (command == NET_SAVE_AUTH || error != ESP_OK) continue;
            }
            dns_checked = false;
            atomic_store(&s_disconnect_reason, 0);
            error = command == NET_PROVISION ? open_portal() : connect_saved();
            connect_until = esp_timer_get_time() + 30000000;
            time_until = 0;
            if (command == NET_PROVISION && error == ESP_OK) portal_until = esp_timer_get_time() + PORTAL_MS * 1000LL;
            if (error != ESP_OK) {
                if (s_server) close_portal();
                if (s_initialized) (void)esp_wifi_stop();
                atomic_store(&s_state, error == ESP_ERR_NVS_NOT_FOUND || error == ESP_ERR_NVS_NOT_INITIALIZED ? MUSIC_NET_OFFLINE : MUSIC_NET_ERROR);
                ESP_LOGW(TAG, "Network operation unavailable: %s", esp_err_to_name(error));
            }
            backoff = 1000; retry_at = esp_timer_get_time() + 5000000;
        }
        if (music_network_state() == MUSIC_NET_PROVISIONING) {
            if (atomic_load(&s_saved) || esp_timer_get_time() >= portal_until) {
                vTaskDelay(pdMS_TO_TICKS(500));
                close_portal(); command = NET_CONNECT; pending = true;
            }
            continue;
        }
        if (!s_initialized || music_network_state() == MUSIC_NET_OFFLINE || music_network_state() == MUSIC_NET_ERROR) continue;
        if (xEventGroupGetBits(s_events) & NET_IP) {
            if (!s_sntp_started) { if (esp_netif_sntp_start() == ESP_OK) s_sntp_started = true; }
            bool ready = music_network_https_ready();
            backoff = 1000;
            connect_until = 0;
            if (!time_until) time_until = esp_timer_get_time() + 60000000;
            if (!ready && esp_timer_get_time() >= time_until) {
                if (music_network_state() != MUSIC_NET_TIME_TIMEOUT)
                    ESP_LOGW(TAG, "SNTP timeout: Wi-Fi connected; DNS/UDP 123 access needs checking. Background retries remain active.");
                atomic_store(&s_state, MUSIC_NET_TIME_TIMEOUT);
            } else {
                if (ready && music_network_state() != MUSIC_NET_READY) {
                    ESP_LOGI(TAG, "Network HTTPS ready");
                    if (!dns_checked) { dns_checked = true; ensure_origin_dns(); }
                }
                atomic_store(&s_state, ready ? MUSIC_NET_READY : MUSIC_NET_CONNECTED);
            }
        } else {
            s_sntp_started = false;
            time_until = 0;
            if (!connect_until) connect_until = esp_timer_get_time() + 30000000;
            if (esp_timer_get_time() >= connect_until) {
                (void)esp_wifi_disconnect();
                atomic_store(&s_state, MUSIC_NET_ERROR);
                continue;
            }
            atomic_store(&s_state, MUSIC_NET_CONNECTING);
            if (esp_timer_get_time() >= retry_at) {
                (void)esp_wifi_connect();
                retry_at = esp_timer_get_time() + (5000 + backoff) * 1000LL;
                if (backoff < 30000) backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
            }
        }
    }
}

esp_err_t music_network_start_saved(void)
{
    if (s_commands) return ESP_ERR_INVALID_STATE;
    s_events = xEventGroupCreate(); s_commands = xQueueCreate(1, sizeof(command_t)); s_info_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_commands || !s_info_lock ||
        xTaskCreate(worker, "music_wifi", 6144, NULL, 3, NULL) != pdPASS) {
        if (s_events) vEventGroupDelete(s_events);
        if (s_commands) vQueueDelete(s_commands);
        if (s_info_lock) vSemaphoreDelete(s_info_lock);
        s_events = NULL; s_commands = NULL; s_info_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool music_network_https_ready(void)
{
    return s_events && music_network_state() != MUSIC_NET_PROVISIONING &&
        (xEventGroupGetBits(s_events) & (NET_IP | NET_TIME)) == (NET_IP | NET_TIME) && time(NULL) >= 1704067200;
}

music_network_state_t music_network_state(void) { return atomic_load(&s_state); }
static bool queue_command(command_t command) { return s_commands && xQueueSend(s_commands, &command, 0) == pdTRUE; }
int music_network_save_status(void) { return atomic_load(&s_save_status); }
int music_network_disconnect_reason(void) { return atomic_load(&s_disconnect_reason); }
bool music_network_scan(void)
{
    if (!s_info_lock) return false;
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    s_scan_count = -1;
    bool ok = queue_command(NET_SCAN);
    if (!ok) s_scan_count = -2;
    xSemaphoreGive(s_info_lock);
    return ok;
}
int music_network_scan_results(char names[12][33])
{
    if (!s_info_lock) return -2;
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    int count = s_scan_count;
    if (count > 0) memcpy(names, s_scan_names, (size_t)count * 33);
    xSemaphoreGive(s_info_lock);
    return count;
}
static bool queue_edit(const char *name, const char *password, bool wifi)
{
    if (!s_info_lock || !name || !password || strlen(name) > (wifi ? 32 : 64) ||
        strlen(password) > (wifi ? 63 : 128)) return false;
    if (wifi ? (!*name || !valid_password(password)) :
        (!auth_string(name, true) || !auth_string(password, false) || (!!*name != !!*password))) return false;
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    bool ok = false;
    if (!s_edit_pending) {
        strcpy(s_edit_name, name); strcpy(s_edit_password, password);
        s_edit_pending = true;
        atomic_store(&s_save_status, 1);
        ok = queue_command(wifi ? NET_SAVE_WIFI : NET_SAVE_AUTH);
        if (!ok) {
            s_edit_pending = false; atomic_store(&s_save_status, -1);
            clear(s_edit_name, sizeof(s_edit_name)); clear(s_edit_password, sizeof(s_edit_password));
        }
    }
    xSemaphoreGive(s_info_lock);
    return ok;
}
bool music_network_save_wifi(const char *ssid, const char *password) { return queue_edit(ssid, password, true); }
bool music_network_save_auth(const char *user, const char *password) { return queue_edit(user, password, false); }
bool music_network_provision(void) { return queue_command(NET_PROVISION); }
bool music_network_reconnect(void) { return queue_command(NET_CONNECT); }
bool music_network_cancel_provision(void) { return queue_command(NET_CANCEL); }
void music_network_ap_info(char *ssid, unsigned ssid_size, char *password, unsigned password_size)
{
    if (!ssid || !password || !ssid_size || !password_size) return;
    ssid[0] = password[0] = 0;
    if (!s_info_lock) return;
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    snprintf(ssid, ssid_size, "%s", s_ap_name); snprintf(password, password_size, "%s", s_ap_password);
    xSemaphoreGive(s_info_lock);
}
