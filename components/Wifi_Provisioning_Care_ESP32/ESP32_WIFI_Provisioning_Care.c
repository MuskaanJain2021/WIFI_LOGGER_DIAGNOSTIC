// SPDX-FileCopyrightText: 2026 Muskaan Jain
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/lock.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "dns_server.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "cJSON.h"
#include "ESP32_WIFI_Provision_Care.h"

static const char *TAG = "wifi_prov_care";

#define MAX_SCAN_RESULTS 15
#define STA_CONNECT_TIMEOUT_MS 30000

// Thread-safe state variables
static wifi_state_t s_current_wifi_state = WIFI_STATE_INIT;
static _lock_t s_state_lock;

static esp_netif_t *s_wifi_sta_netif = NULL;
static esp_netif_t *s_wifi_ap_netif = NULL;
static httpd_handle_t s_webserver_handle = NULL;
static SemaphoreHandle_t s_semph_get_ip_addrs = NULL;
// Add this near the top of the file with the other static variables
static dns_server_handle_t s_dns_server_handle = NULL;
static char s_ap_ssid_name_copy[32] = {0};
static bool s_softap_initialized = false;
static bool s_retry_task_running = false;
static int s_retry_num = 0;

// Forward Declarations of Local Helpers
static void set_wifi_state(wifi_state_t new_state);
static void wifi_init_softap(void);
static void stop_provisioning_services(void);

// Global Linker Symbols for Embedded Web Assets
extern const uint8_t wifi_html_gz_start[] asm("_binary_wifi_html_gz_start");
extern const uint8_t wifi_html_gz_end[] asm("_binary_wifi_html_gz_end");
extern const uint8_t favicon_ico_start[] asm("_binary_esp32_wifi_provision_care_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_esp32_wifi_provision_care_favicon_ico_end");

// MARK: - Core Infrastructure Subsystem Infrastructure
static void stop_provisioning_services(void)
{
    if (s_webserver_handle)
    {
        httpd_stop(s_webserver_handle);
        s_webserver_handle = NULL;
        ESP_LOGI(TAG, "Captive HTTP Server destroyed.");
    }

    // FIX: Pass the tracking handle instead of invoking it with empty arguments
    if (s_dns_server_handle)
    {
        stop_dns_server(s_dns_server_handle);
        s_dns_server_handle = NULL;
    }
}

static void delayed_restart_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    stop_provisioning_services();
    esp_wifi_stop();
    esp_restart();
    vTaskDelete(NULL); // Task functions must always delete themselves
}

static void ota_reboot_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    stop_provisioning_services();
    esp_wifi_stop();
    esp_restart();
    vTaskDelete(NULL);
}

static void prov_watchdog_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000)); // 10 minutes
    ESP_LOGE(TAG, "Watchdog timeout processing provisioning data. Resetting hardware...");
    esp_wifi_stop();
    esp_restart();
    vTaskDelete(NULL);
}

// MARK: - Thread-Safe State Management

static void set_wifi_state(wifi_state_t new_state)
{
    _lock_acquire(&s_state_lock);
    s_current_wifi_state = new_state;
    _lock_release(&s_state_lock);
    ESP_LOGI(TAG, "Subsystem State Transitioned to Enum: %d", new_state);
}

wifi_state_t wifi_provision_care_get_state(void)
{
    _lock_acquire(&s_state_lock);
    wifi_state_t state = s_current_wifi_state;
    _lock_release(&s_state_lock);
    return state;
}

esp_netif_t *wifi_provision_care_get_sta_netif(void)
{
    return s_wifi_sta_netif;
}

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    size_t j = 0;

    while (src[i] != '\0' && j < dst_size - 1)
    {
        if (src[i] == '%' && src[i + 1] && src[i + 2])
        {
            int hi = hex_to_int(src[i + 1]);
            int lo = hex_to_int(src[i + 2]);

            if (hi >= 0 && lo >= 0)
            {
                dst[j++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }

        if (src[i] == '+')
        {
            dst[j++] = ' ';
            i++;
            continue;
        }

        dst[j++] = src[i++];
    }

    dst[j] = '\0';
}

// MARK: - HTTP Server URI Handlers

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    const size_t size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    return httpd_resp_send(req, (const char *)favicon_ico_start, size);
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (!s_wifi_ap_netif)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_OK;
    }

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_wifi_ap_netif, &ip_info));

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));

    char redirect_uri[48];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://%s/wifi", ip_addr);

    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", redirect_uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, "Redirecting to captive portal...", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const uint32_t len = wifi_html_gz_end - wifi_html_gz_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)wifi_html_gz_start, len);
}

static esp_err_t nvserase_get_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory Reset Endpoint Invoked. Erasing Flash...");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "NVS Erase Initiated. Device Rebooting...", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_stop();
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}
static esp_err_t savewifi_get_handler(httpd_req_t *req)
{
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;

    if (buf_len <= 1 || buf_len > 256)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query");
        return ESP_FAIL;
    }

    char *buf = calloc(1, buf_len);
    if (buf == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, buf, buf_len) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read query");
        return ESP_FAIL;
    }

    char ssid_encoded[96] = {0};
    char pass_encoded[96] = {0};

    char ssid_param[65] = {0};
    char password_param[65] = {0};

    wifi_config_t wifi_cfg = {0};

    if (httpd_query_key_value(buf, "ssid", ssid_encoded, sizeof(ssid_encoded)) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    httpd_query_key_value(buf, "password", pass_encoded, sizeof(pass_encoded));

    free(buf);

    url_decode(ssid_param, ssid_encoded, sizeof(ssid_param));
    url_decode(password_param, pass_encoded, sizeof(password_param));

    if (strlen(ssid_param) == 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty SSID");
        return ESP_FAIL;
    }

    strncpy((char *)wifi_cfg.sta.ssid,
            ssid_param,
            sizeof(wifi_cfg.sta.ssid) - 1);

    strncpy((char *)wifi_cfg.sta.password,
            password_param,
            sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write Wi-Fi config: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi save failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wi-Fi settings saved, SSID: '%s'", ssid_param);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "Wi-Fi settings saved. Device rebooting...", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task,
                "delayed_restart",
                2048,
                NULL,
                tskIDLE_PRIORITY,
                NULL);

    return ESP_OK;
}
static esp_err_t savewifi_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 256)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Payload Length");
        return ESP_FAIL;
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Heap Exhaustion");
        return ESP_FAIL;
    }

    int received_total = 0;
    while (received_total < req->content_len)
    {
        int received = httpd_req_recv(req, buf + received_total, req->content_len - received_total);
        if (received <= 0)
        {
            free(buf);
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Receive Timeout");
            }
            else
            {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Socket Error");
            }
            return ESP_FAIL;
        }
        received_total += received;
    }
    buf[received_total] = '\0';

    char ssid_param[65] = {0};
    char password_param[65] = {0};
    wifi_config_t wifi_cfg = {0};

    if (httpd_query_key_value(buf, "ssid", ssid_param, sizeof(ssid_param)) != ESP_OK || strlen(ssid_param) == 0)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or Empty SSID Parameter");
        return ESP_FAIL;
    }
    httpd_query_key_value(buf, "password", password_param, sizeof(password_param));
    free(buf);

    strncpy((char *)wifi_cfg.sta.ssid, ssid_param, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password_param, sizeof(wifi_cfg.sta.password) - 1);

    set_wifi_state(WIFI_STATE_RESTART_REQUIRED);

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit flash credentials: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash Write Failure");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Target configuration committed safely. Device rebooting in 3 seconds.");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "Settings Secured Successfully. Unit System Restarting...", HTTPD_RESP_USE_STRLEN);

    // call the separate static worker function we defined earlier
    xTaskCreate(delayed_restart_task, "delayed_restart", 2048, NULL, tskIDLE_PRIORITY, NULL);

    return ESP_OK;
}
static int compare_ap_by_rssi_desc(const void *a, const void *b)
{
    return (int)((wifi_ap_record_t *)b)->rssi - (int)((wifi_ap_record_t *)a)->rssi;
}

static esp_err_t scanap_get_handler(httpd_req_t *req)
{
    uint16_t number = MAX_SCAN_RESULTS;
    wifi_ap_record_t ap_info[MAX_SCAN_RESULTS] = {0};
    uint16_t ap_count = 0;

    wifi_scan_config_t scan_config = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};

    // Safe background operational scanning hook validation
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RF Peripheral busy, scan rejected: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan Rejected");
        return ESP_FAIL;
    }

    esp_wifi_scan_get_ap_num(&ap_count);
    esp_wifi_scan_get_ap_records(&number, ap_info);
    qsort(ap_info, number, sizeof(wifi_ap_record_t), compare_ap_by_rssi_desc);

    cJSON *root = cJSON_CreateArray();
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Allocation Failed");
        return ESP_FAIL;
    }

    for (uint16_t i = 0; i < number; i++)
    {
        if (ap_info[i].ssid[0] == '\0')
            continue;

        cJSON *ap_obj = cJSON_CreateObject();
        if (!ap_obj)
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM processing scan items");
            return ESP_FAIL;
        }

        cJSON_AddStringToObject(ap_obj, "ssid", (char *)ap_info[i].ssid);
        cJSON_AddNumberToObject(ap_obj, "rssi", ap_info[i].rssi);
        cJSON_AddNumberToObject(ap_obj, "auth", ap_info[i].authmode);
        cJSON_AddItemToArray(root, ap_obj);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON serialization failure");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t resp_err = httpd_resp_sendstr(req, json_str);
    free(json_str);

    return resp_err;
}
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting http server on port: %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        const httpd_uri_t favicon_uri = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler};
        httpd_register_uri_handler(server, &favicon_uri);
        const httpd_uri_t root_uri = {.uri = "/wifi", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &root_uri);
        const httpd_uri_t scanap_uri = {.uri = "/scanap", .method = HTTP_GET, .handler = scanap_get_handler};
        httpd_register_uri_handler(server, &scanap_uri);
        const httpd_uri_t savewifi_uri = {.uri = "/savewifi", .method = HTTP_GET, .handler = savewifi_get_handler};
        httpd_register_uri_handler(server, &savewifi_uri);
        const httpd_uri_t nvserase_uri = {.uri = "/nvserase", .method = HTTP_GET, .handler = nvserase_get_handler};
        httpd_register_uri_handler(server, &nvserase_uri);
        const httpd_uri_t updateota_uri = {.uri = "/updateota", .method = HTTP_POST, .handler = wifi_provision_care_updateota_post_handler};
        httpd_register_uri_handler(server, &updateota_uri);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
    }
    return server;
}

// MARK: - OTA Firmware Engine (Hardened against Timeout crashing)

esp_err_t wifi_provision_care_updateota_post_handler(httpd_req_t *req)
{
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Target flash partitioning table lacks OTA spaces");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > update_partition->size)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware binary footprint payload sizing mismatch");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA Flash Block lock failure");
        return ESP_FAIL;
    }

    char *buf = malloc(5800);
    if (!buf)
    {
        esp_ota_abort(update_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Heap exhaustion allocating stream buffer");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int consecutive_timeouts = 0;

    while (remaining > 0)
    {
        int received = httpd_req_recv(req, buf, (remaining > 5800 ? 5800 : remaining));
        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                if (++consecutive_timeouts > 5)
                {
                    ESP_LOGE(TAG, "Aborting streaming OTA download due to network timeouts");
                    break;
                }
                continue;
            }
            break;
        }
        consecutive_timeouts = 0;

        err = esp_ota_write(update_handle, (const void *)buf, received);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Flash page write abort: %s", esp_err_to_name(err));
            break;
        }
        remaining -= received;
    }
    free(buf);

    if (remaining > 0)
    {
        esp_ota_abort(update_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA flash streaming interrupted");
        return ESP_FAIL;
    }

    if (esp_ota_end(update_handle) != ESP_OK || esp_ota_set_boot_partition(update_partition) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image verification / checksum failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "Firmware updated seamlessly. Rebooting system structural blocks...", HTTPD_RESP_USE_STRLEN);

    // Find this block at the end of the OTA handler and replace it:
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "Firmware updated seamlessly. Rebooting system structural blocks...", HTTPD_RESP_USE_STRLEN);

    // FIX: Change the lambda to a clean C task create call
    xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, tskIDLE_PRIORITY, NULL);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_wifi_stop();
    esp_restart();

    return ESP_OK;
}
static void wifi_init_softap(void)
{
    if (s_softap_initialized)
        return;

    ESP_LOGW(TAG, "Entering fallback mode: Constructing Safe Provisioning Access Point...");
    set_wifi_state(WIFI_STATE_PROVISIONING);

    esp_wifi_stop(); // Hops operational states cleanly inside native driver system

    if (!s_wifi_ap_netif)
    {
        s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_config = {
        .ap = {.password = "", .max_connection = 4, .authmode = WIFI_AUTH_OPEN}};

    if (strlen(s_ap_ssid_name_copy) > 0)
    {
        strncpy((char *)ap_config.ap.ssid, s_ap_ssid_name_copy, sizeof(ap_config.ap.ssid));
    }
    else
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
        snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "ESP32-PROV-%02X%02X", mac[4], mac[5]);
    }

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_wifi_ap_netif));
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 200, 200, 200, 2);
    IP4_ADDR(&ip_info.gw, 200, 200, 200, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_wifi_ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_wifi_ap_netif));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_softap_initialized = true;

    // Direct Web Server initialization inline inside executing safe task thread boundary context
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;

    if (httpd_start(&s_webserver_handle, &config) == ESP_OK)
    {
        const httpd_uri_t uris[] = {
            {"/favicon.ico", HTTP_GET, favicon_get_handler, NULL},
            {"/wifi", HTTP_GET, root_get_handler, NULL},
            {"/scanap", HTTP_GET, scanap_get_handler, NULL},
            {"/savewifi", HTTP_GET, savewifi_get_handler, NULL},
            {"/nvserase", HTTP_GET, nvserase_get_handler, NULL},
            {"/updateota", HTTP_POST, wifi_provision_care_updateota_post_handler, NULL}};
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        {
            httpd_register_uri_handler(s_webserver_handle, &uris[i]);
        }
        httpd_register_err_handler(s_webserver_handle, HTTPD_404_NOT_FOUND, http_404_error_handler);
    }

    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns_server_handle = start_dns_server(&dns_config);

    // FIX: Spawn background tracking watchdog and return cleanly without blocking execution flow!
    xTaskCreate(prov_watchdog_task, "prov_wdog", 2048, NULL, tskIDLE_PRIORITY, NULL);
}
// MARK: - FreeRTOS Decoupled Async Operational Handlers

static void wifi_retry_task(void *param)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    _lock_acquire(&s_state_lock);
    if (s_current_wifi_state == WIFI_STATE_STA_CONNECTING)
    {
        ESP_LOGI(TAG, "Re-issuing system level connection request to peripheral driver...");
        esp_wifi_connect();
    }
    s_retry_task_running = false;
    _lock_release(&s_state_lock);
    vTaskDelete(NULL);
}

static void sta_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (id == WIFI_EVENT_STA_DISCONNECTED)
    {
        _lock_acquire(&s_state_lock);

        // Stop attempting to re-connect if fallback has already been initialized
        if (s_current_wifi_state == WIFI_STATE_PROVISIONING || s_current_wifi_state == WIFI_STATE_RESTART_REQUIRED)
        {
            _lock_release(&s_state_lock);
            return;
        }

        s_retry_num++;
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Disconnect detected. Disconnect Reason Code: %d, Current Retry Iteration: %d/5", disconn->reason, s_retry_num);

        if (s_retry_num > 5)
        {
            ESP_LOGE(TAG, "Maximum fallback connection threshold reached. Initializing SoftAP workflow...");
            if (s_semph_get_ip_addrs)
            {
                xSemaphoreGive(s_semph_get_ip_addrs); // Instantly unlock primary blocked calling task thread channel
            }
        }
        else if (!s_retry_task_running)
        {
            s_retry_task_running = true;
            xTaskCreate(wifi_retry_task, "wifi_retry", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
        }
        _lock_release(&s_state_lock);
    }
}

static void sta_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Leased IP from router system space: " IPSTR, IP2STR(&event->ip_info.ip));

        _lock_acquire(&s_state_lock);
        if (s_semph_get_ip_addrs)
        {
            xSemaphoreGive(s_semph_get_ip_addrs);
        }
        _lock_release(&s_state_lock);
    }
}

// MARK: - Core Public API Entry Point

void wifi_provision_care(const char ap_ssid_name[32])
{
    _lock_acquire(&s_state_lock);

    if (s_current_wifi_state != WIFI_STATE_INIT)
    {
        _lock_release(&s_state_lock);
        ESP_LOGW(TAG, "Subsystem API execution bypassed: Module already instantiated.");
        return;
    }

    s_current_wifi_state = WIFI_STATE_STA_CONNECTING;

    _lock_release(&s_state_lock);

    ESP_LOGI(TAG, "Subsystem State Transitioned to Enum: %d", WIFI_STATE_STA_CONNECTING);

    s_semph_get_ip_addrs = xSemaphoreCreateBinary();
    assert(s_semph_get_ip_addrs != NULL);

    memset(s_ap_ssid_name_copy, 0, sizeof(s_ap_ssid_name_copy));
    if (ap_ssid_name && ap_ssid_name[0] != '\0')
    {
        strncpy(s_ap_ssid_name_copy, ap_ssid_name, sizeof(s_ap_ssid_name_copy) - 1);
    }

    // Guarded Singleton Initializations (Crash-Proof Re-entry)
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(err);

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(err);

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    assert(s_wifi_sta_netif != NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    wifi_config_t wifi_config = {0};
    bool system_has_valid_credentials = true;

    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) != ESP_OK || strlen((const char *)wifi_config.sta.ssid) == 0)
    {
        system_has_valid_credentials = false;
        ESP_LOGE(TAG, "NVS space lacks valid target profiles.");
    }

    if (!system_has_valid_credentials)
    {
        vSemaphoreDelete(s_semph_get_ip_addrs);
        s_semph_get_ip_addrs = NULL;
        wifi_init_softap();
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Secure sync timeout point
    if (xSemaphoreTake(s_semph_get_ip_addrs, pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to establish station hook within time window allocation limits.");

        // Critical step: Stop the background driver connecting attempts safely
        esp_wifi_stop();

        _lock_acquire(&s_state_lock);
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_ip_event_handler);
        vSemaphoreDelete(s_semph_get_ip_addrs);
        s_semph_get_ip_addrs = NULL;
        _lock_release(&s_state_lock);

        wifi_init_softap();
        return;
    }

    // Validate operational final conditions safely
    _lock_acquire(&s_state_lock);

    if (s_retry_num > 5)
    {
        esp_wifi_stop();

        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_ip_event_handler);

        vSemaphoreDelete(s_semph_get_ip_addrs);
        s_semph_get_ip_addrs = NULL;

        _lock_release(&s_state_lock);

        wifi_init_softap();
        return;
    }

    // Do not call set_wifi_state() while lock is already held
    s_current_wifi_state = WIFI_STATE_STA_CONNECTED;

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_ip_event_handler);

    vSemaphoreDelete(s_semph_get_ip_addrs);
    s_semph_get_ip_addrs = NULL;

    _lock_release(&s_state_lock);

    ESP_LOGI(TAG, "Subsystem State Transitioned to Enum: %d", WIFI_STATE_STA_CONNECTED);

    if (s_webserver_handle == NULL)
    {
        s_webserver_handle = start_webserver();

        if (s_webserver_handle != NULL)
        {
            ESP_LOGI(TAG, "HTTP server started in STA mode. Open: http://<ESP32-IP>/wifi");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to start HTTP server in STA mode.");
        }
    }

    ESP_LOGI(TAG, "Provisioning framework detached. Device safely connected.");
}
