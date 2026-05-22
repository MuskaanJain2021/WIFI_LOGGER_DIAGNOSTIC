#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // Forward declaration to prevent header bloat across your firmware components
    struct httpd_req;
    typedef struct httpd_req httpd_req_t;
    struct esp_netif_obj;
    typedef struct esp_netif_obj esp_netif_t;

    /**
     * @brief Thread-safe operational states of the Provision Care subsystem.
     */
    typedef enum
    {
        WIFI_STATE_INIT,            /* Subsystem initialized, awaiting driver bring-up */
        WIFI_STATE_STA_CONNECTING,  /* Attempting to connect to stored credentials */
        WIFI_STATE_STA_CONNECTED,   /* Connected successfully, IPv4/IPv6 assigned */
        WIFI_STATE_PROVISIONING,    /* Fallback triggered: Captive Portal and SoftAP active */
        WIFI_STATE_RESTART_REQUIRED /* Credentials saved; waiting for scheduled system reboot */
    } wifi_state_t;

    /**
     * @brief Tries to connect to the Wi-Fi Access Point using credentials stored in flash (NVS).
     *        If connection fails or credentials are empty, falls back to SoftAP captive portal.
     *
     * @note This function is non-blocking regarding the SoftAP lifecycle; if a fallback is
     *       triggered, it offloads execution to an isolated, safe worker thread.
     *
     * @param[in] ap_ssid_name Expects a null-terminated string up to 32 bytes.
     *                         If NULL or empty, defaults to "esp32-wifi-provision-care-XXXX".
     */
    void wifi_provision_care(const char ap_ssid_name[32]);

    /**
     * @brief Thread-safe runtime state query interface.
     *
     * @return Current active wifi_state_t state machine value.
     */
    wifi_state_t wifi_provision_care_get_state(void);

    /**
     * @brief Public accessor to retrieve the native Station network interface pointer.
     *        Useful for manual IP status monitoring, SNTP Sync, or socket configurations.
     *
     * @return Pointer to active esp_netif_t instance, or NULL if uninitialized.
     */
    esp_netif_t *wifi_provision_care_get_sta_netif(void);

    /**
     * @brief HTTP POST handler designed to process streaming binary multipart firmware blocks over-the-air.
     *        Registers rollback validation logic to secure the system against corrupted images.
     *
     * @example
     * const httpd_uri_t ota_uri = {
     *     .uri      = "/updateota",
     *     .method   = HTTP_POST,
     *     .handler  = wifi_provision_care_updateota_post_handler,
     *     .user_ctx = NULL
     * };
     * httpd_register_uri_handler(server, &ota_uri);
     *
     * @param[in] req Pointer to the raw ESP-IDF httpd request context block.
     * @return ESP_OK on seamless flash burn, or native ESP_ERR_OTA codes on operational failure.
     */
    esp_err_t wifi_provision_care_updateota_post_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif