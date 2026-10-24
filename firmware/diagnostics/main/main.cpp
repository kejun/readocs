// PaperDIY board initialization follows m5stack/M5PaperDIY-UserDemo (MIT).
// Connectivity follows the ESP-IDF station/SoftAP examples. No embedded Wi-Fi credentials.
#include <M5Unified.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cJSON.h>
#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <nvs.h>
#include <nvs_flash.h>

static constexpr char TAG[] = "readocs-check";
static EventGroupHandle_t events;
static QueueHandle_t credentials;
static esp_netif_t *sta_netif, *ap_netif;
static std::atomic<bool> connected{false}, redraw{true}, dns_ok{false}, time_ok{false};
static std::atomic<int> battery_mv{0}, https_status{0}, disconnect_reason{0};
static std::atomic<unsigned> button_a{0}, button_b{0};
static bool psram_ok = false, display_ok = false;
static char setup_ssid[32], setup_password[17];
static constexpr EventBits_t GOT_IP = BIT0, LOST_IP = BIT1;

static cJSON* status_json()
{
    auto* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "firmware", "readocs-check-0.1.0");
    cJSON_AddStringToObject(j, "board", "PaperDIY");
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddBoolToObject(j, "connected", connected.load());
    esp_netif_ip_info_t info{};
    char ip[16]{};
    esp_netif_get_ip_info(sta_netif, &info);
    std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    cJSON_AddStringToObject(j, "ip", ip);
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(j, "ssid", reinterpret_cast<const char*>(ap.ssid));
        cJSON_AddNumberToObject(j, "rssi_dbm", ap.rssi);
        cJSON_AddNumberToObject(j, "channel", ap.primary);
    }
    cJSON_AddNumberToObject(j, "disconnect_reason", disconnect_reason.load());
    cJSON_AddBoolToObject(j, "psram_pattern_ok", psram_ok);
    cJSON_AddNumberToObject(j, "psram_bytes", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddBoolToObject(j, "display_initialized", display_ok);
    cJSON_AddNumberToObject(j, "battery_mv", battery_mv.load());
    cJSON_AddNumberToObject(j, "button_a_presses", button_a.load());
    cJSON_AddNumberToObject(j, "button_b_presses", button_b.load());
    cJSON_AddBoolToObject(j, "dns_ok", dns_ok.load());
    cJSON_AddBoolToObject(j, "sntp_ok", time_ok.load());
    cJSON_AddNumberToObject(j, "https_status", https_status.load());
    return j;
}

static void print_status()
{
    cJSON* j = status_json();
    char* s = cJSON_PrintUnformatted(j);
    if (s) { std::printf("STATUS %s\n", s); cJSON_free(s); }
    cJSON_Delete(j);
}

static esp_err_t submit_wifi(cJSON* j)
{
    auto* ssid = cJSON_GetObjectItemCaseSensitive(j, "ssid");
    auto* pass = cJSON_GetObjectItemCaseSensitive(j, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) return ESP_ERR_INVALID_ARG;
    size_t sn = std::strlen(ssid->valuestring), pn = std::strlen(pass->valuestring);
    if (sn == 0 || sn > 32 || (pn != 0 && (pn < 8 || pn > 63))) return ESP_ERR_INVALID_ARG;
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, ssid->valuestring, sn);
    std::memcpy(config.sta.password, pass->valuestring, pn);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = pn ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    bool queued = xQueueSend(credentials, &config, 0) == pdTRUE;
    std::memset(&config, 0, sizeof(config));
    return queued ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void wifi_event(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        disconnect_reason = 0;
        xEventGroupSetBits(events, GOT_IP);
        auto* got = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "DHCP IP: " IPSTR, IP2STR(&got->ip_info.ip));
        redraw = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        auto* lost = static_cast<wifi_event_sta_disconnected_t*>(data);
        disconnect_reason = lost->reason;
        xEventGroupSetBits(events, LOST_IP);
        ESP_LOGW(TAG, "Disconnected, reason=%u", lost->reason);
        redraw = true;
    }
}

static void network_checks()
{
    struct addrinfo* result = nullptr;
    dns_ok = getaddrinfo("example.com", nullptr, nullptr, &result) == 0;
    if (result) freeaddrinfo(result);
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        time_ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) == ESP_OK;
        esp_netif_sntp_deinit();
    }
    esp_http_client_config_t hc{};
    hc.url = "https://example.com/";
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    hc.timeout_ms = 15000;
    auto client = esp_http_client_init(&hc);
    if (client) {
        err = esp_http_client_perform(client);
        https_status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
        ESP_LOGI(TAG, "HTTPS result=%s status=%d", esp_err_to_name(err), https_status.load());
        esp_http_client_cleanup(client);
    }
    redraw = true;
    print_status();
}

static void network_task(void*)
{
    wifi_config_t config{};
    while (true) {
        if (xQueueReceive(credentials, &config, portMAX_DELAY) != pdTRUE) continue;
        dns_ok = false; time_ok = false; https_status = 0;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
        xEventGroupClearBits(events, GOT_IP | LOST_IP);
        esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (error == ESP_OK) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                xEventGroupClearBits(events, GOT_IP | LOST_IP);
                error = esp_wifi_connect();
                if (error != ESP_OK) break;
                auto bits = xEventGroupWaitBits(events, GOT_IP | LOST_IP, pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
                if (bits & GOT_IP) break;
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (connected.load()) {
            nvs_handle_t nvs;
            if (nvs_open("readocs_net", NVS_READWRITE, &nvs) == ESP_OK) {
                char name[33]{};
                std::memcpy(name, config.sta.ssid, 32);
                esp_err_t saved = nvs_set_str(nvs, "ssid", name);
                if (saved == ESP_OK) saved = nvs_set_str(nvs, "pass", reinterpret_cast<char*>(config.sta.password));
                if (saved == ESP_OK) saved = nvs_commit(nvs);
                ESP_LOGI(TAG, "Credential persistence: %s", esp_err_to_name(saved));
                nvs_close(nvs);
            }
            // Provisioning is physically available again on reboot; do not leave an AP on after success.
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
            std::memset(&config, 0, sizeof(config));
            network_checks();
        } else {
            ESP_LOGE(TAG, "Connection failed: %s; reason=%d", esp_err_to_name(error), disconnect_reason.load());
            std::memset(&config, 0, sizeof(config));
            print_status();
        }
    }
}

static const char PAGE[] = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Readocs 设备配网</title><style>body{font:18px system-ui;max-width:480px;margin:48px auto;padding:20px}input,button{display:block;box-sizing:border-box;width:100%;padding:12px;margin:12px 0}pre{white-space:pre-wrap;font-size:14px}</style><h1>Readocs 设备配网</h1><p>连接设备屏幕上的热点后，填写 2.4 GHz Wi-Fi。</p><form id="f"><label>Wi-Fi 名称<input name="ssid" maxlength="32" required></label><label>密码<input name="password" type="password" maxlength="63" autocomplete="new-password"></label><button>连接</button></form><pre id="s"></pre><script>let s=document.querySelector('#s');document.querySelector('#f').onsubmit=async e=>{e.preventDefault();let data=Object.fromEntries(new FormData(e.target));let r=await fetch('/api/provision',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});data.password='';s.textContent=await r.text();e.target.password.value='';};async function status(){try{s.textContent=JSON.stringify(await(await fetch('/api/status')).json(),null,2)}catch{}}status();</script></html>)HTML";

static esp_err_t web_handler(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (req->method == HTTP_GET && std::strcmp(req->uri, "/") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
    }
    if (req->method == HTTP_GET) {
        auto* j = status_json();
        char* s = cJSON_PrintUnformatted(j);
        httpd_resp_set_type(req, "application/json");
        auto err = s ? httpd_resp_sendstr(req, s) : ESP_ERR_NO_MEM;
        cJSON_free(s); cJSON_Delete(j);
        return err;
    }
    // Only the WPA2-protected setup interface accepts Wi-Fi changes.
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    esp_netif_ip_info_t ap{};
    esp_netif_get_ip_info(ap_netif, &ap);
    if (getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr*>(&local), &len) != 0 ||
        local.sin_addr.s_addr != ap.ip.addr) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Connect to the device setup hotspot first");
    }
    char body[512]{};
    if (req->content_len <= 0 || req->content_len >= static_cast<int>(sizeof(body)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n <= 0) return ESP_FAIL;
        total += n;
    }
    auto* j = cJSON_Parse(body);
    esp_err_t err = submit_wifi(j);
    cJSON_Delete(j); std::memset(body, 0, sizeof(body));
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi data or connection busy");
    return httpd_resp_sendstr(req, "已开始连接，请查看设备屏幕。连接成功后配网热点将关闭。");
}

static void console_task(void*)
{
    char line[512]{};
    size_t used = 0;
    bool overflow = false;
    while (true) {
        char ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (ch != '\n' && ch != '\r') {
            if (used < sizeof(line) - 1) line[used++] = ch; else overflow = true;
            continue;
        }
        if (!used && !overflow) continue;
        line[used] = 0;
        auto* j = overflow ? nullptr : cJSON_Parse(line);
        auto* cmd = cJSON_GetObjectItemCaseSensitive(j, "cmd");
        if (cJSON_IsString(cmd) && std::strcmp(cmd->valuestring, "wifi") == 0)
            std::printf("WIFI_ACCEPTED %s\n", esp_err_to_name(submit_wifi(j)));
        else if (cJSON_IsString(cmd) && std::strcmp(cmd->valuestring, "status") == 0) print_status();
        else if (cJSON_IsString(cmd) && std::strcmp(cmd->valuestring, "setup") == 0) {
            if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK)
                std::printf("SETUP {\"ssid\":\"%s\",\"password\":\"%s\",\"ip\":\"192.168.4.1\"}\n", setup_ssid, setup_password);
        }
        else if (cJSON_IsString(cmd) && std::strcmp(cmd->valuestring, "reboot") == 0) {
            cJSON_Delete(j); esp_restart();
        } else std::printf("ERROR invalid command\n");
        cJSON_Delete(j); std::memset(line, 0, sizeof(line)); used = 0; overflow = false;
    }
}

static void button_task(void*)
{
    // Same GPIOs and separate polling-task approach as the factory HAL.
    // Full e-paper waveforms can block the main task for several seconds.
    unsigned stable = 0, candidate = 0;
    TickType_t since = xTaskGetTickCount();
    while (true) {
        const unsigned raw = (gpio_get_level(GPIO_NUM_4) == 0 ? 1U : 0U) |
                             (gpio_get_level(GPIO_NUM_3) == 0 ? 2U : 0U);
        const auto now = xTaskGetTickCount();
        if (raw != candidate) { candidate = raw; since = now; }
        else if (candidate != stable && now - since >= pdMS_TO_TICKS(15)) {
            const unsigned pressed = candidate & ~stable;
            stable = candidate;
            if (pressed & 1U) { ++button_a; redraw = true; ESP_LOGI(TAG, "Button A pressed"); }
            if (pressed & 2U) { ++button_b; redraw = true; ESP_LOGI(TAG, "Button B pressed"); }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void draw_screen()
{
    if (!display_ok) return;
    auto& d = M5.Display;
    d.fillScreen(TFT_WHITE);
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setFont(&fonts::Font2);
    d.setTextSize(2);
    d.setCursor(20, 25);
    d.println("READOCS DEVICE CHECK");
    d.printf("\nPaperDIY 540 x 960\nPSRAM test: %s\nBattery: %d mV\n\n", psram_ok ? "PASS" : "FAIL", battery_mv.load());
    if (connected.load()) {
        esp_netif_ip_info_t info{};
        esp_netif_get_ip_info(sta_netif, &info);
        d.printf("Wi-Fi: CONNECTED\nIP: " IPSTR "\n\n", IP2STR(&info.ip));
        d.printf("DNS: %s\nTime sync: %s\nHTTPS: %d\n", dns_ok ? "PASS" : "pending", time_ok ? "PASS" : "pending", https_status.load());
    } else {
        d.printf("Wi-Fi: awaiting setup\n\nAP: %s\nKey: %s\n\nhttp://192.168.4.1\n", setup_ssid, setup_password);
        if (disconnect_reason) d.printf("Reason: %d\n", disconnect_reason.load());
    }
    d.printf("\nButton A: %u\nButton B: %u\n", button_a.load(), button_b.load());
    for (int i = 0; i < 16; ++i) d.fillRect(20 + i * 30, 820, 30, 70, d.color888(i * 17, i * 17, i * 17));
    d.display(); d.waitDisplay();
    ESP_LOGI(TAG, "Display refresh completed");
}

extern "C" void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init()); // Never erase factory data automatically.
    auto cfg = M5.config();
    cfg.clear_display = false; cfg.internal_imu = false; cfg.internal_mic = false; cfg.internal_spk = false;
    M5.begin(cfg);
    display_ok = M5.getDisplayCount() > 0 && M5.Display.width() == 540 && M5.Display.height() == 960;
    M5.Display.setAutoDisplay(false);
    M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
    constexpr size_t TEST_SIZE = 1024 * 1024;
    auto* mem = static_cast<uint8_t*>(heap_caps_malloc(TEST_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    psram_ok = mem != nullptr;
    if (mem) {
        for (size_t i = 0; i < TEST_SIZE; ++i) mem[i] = static_cast<uint8_t>((i * 31U + (i >> 8)) ^ 0xA5);
        for (size_t i = 0; i < TEST_SIZE; ++i) if (mem[i] != static_cast<uint8_t>((i * 31U + (i >> 8)) ^ 0xA5)) { psram_ok = false; break; }
        heap_caps_free(mem);
    }
    battery_mv = M5.Power.M5pm1.getBatteryVoltage();
    ESP_LOGI(TAG, "Board=PaperDIY display=%s PSRAM pattern=%s battery=%dmV", display_ok ? "OK" : "FAIL", psram_ok ? "PASS" : "FAIL", battery_mv.load());
    events = xEventGroupCreate();
    credentials = xQueueCreate(1, sizeof(wifi_config_t));
    assert(events && credentials);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, "readocs-check"));
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    std::snprintf(setup_ssid, sizeof(setup_ssid), "Readocs-Setup-%02X%02X", mac[4], mac[5]);
    std::snprintf(setup_password, sizeof(setup_password), "%08lx%08lx", static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    wifi_config_t ap{};
    std::strcpy(reinterpret_cast<char*>(ap.ap.ssid), setup_ssid);
    std::strcpy(reinterpret_cast<char*>(ap.ap.password), setup_password);
    ap.ap.ssid_len = std::strlen(setup_ssid); ap.ap.channel = 1; ap.ap.max_connection = 2; ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    httpd_handle_t server;
    httpd_config_t sc = HTTPD_DEFAULT_CONFIG(); sc.stack_size = 6144;
    ESP_ERROR_CHECK(httpd_start(&server, &sc));
    for (const auto* uri : {"/", "/api/status", "/api/provision"}) {
        httpd_uri_t route{}; route.uri = uri; route.method = std::strcmp(uri, "/api/provision") == 0 ? HTTP_POST : HTTP_GET; route.handler = web_handler;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
    }
    usb_serial_jtag_driver_config_t usb{.tx_buffer_size = 4096, .rx_buffer_size = 1024};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    ESP_ERROR_CHECK(xTaskCreate(console_task, "usb_commands", 6144, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(network_task, "network_checks", 10240, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(button_task, "button_input", 2048, nullptr, 5, nullptr, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    nvs_handle_t nvs;
    if (nvs_open("readocs_net", NVS_READONLY, &nvs) == ESP_OK) {
        char name[33]{}, pass[64]{}; size_t sn = sizeof(name), pn = sizeof(pass);
        if (nvs_get_str(nvs, "ssid", name, &sn) == ESP_OK && nvs_get_str(nvs, "pass", pass, &pn) == ESP_OK) {
            auto* j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "ssid", name); cJSON_AddStringToObject(j, "password", pass);
            submit_wifi(j); cJSON_Delete(j); std::memset(pass, 0, sizeof(pass));
        }
        nvs_close(nvs);
    }
    std::printf("READY readocs-check-0.1.0; USB JSON commands: wifi/status/setup/reboot\n");
    unsigned ticks = 0;
    while (true) {
        M5.update();
        if (++ticks % 500 == 0) battery_mv = M5.Power.M5pm1.getBatteryVoltage();
        if (redraw.exchange(false)) draw_screen();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
