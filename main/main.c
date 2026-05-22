#include "ESP32_WIFI_Provision_Care.h"
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <mdns.h>

static const char *TAG = "esp32-wifi-provision-care-example";

// MARK: NVS stats
static void nvs_list_all(void)
{
    const char *TAG_LOCAL = __func__;

    nvs_stats_t nvs_stats;
    ESP_ERROR_CHECK(nvs_get_stats(NULL, &nvs_stats));

    ESP_LOGI(TAG_LOCAL,
             "NVS: UsedEntries = %u, FreeEntries = %u, AvailableEntries = %u, AllEntries = %u, AllNamespaces = %u",
             nvs_stats.used_entries,
             nvs_stats.free_entries,
             nvs_stats.available_entries,
             nvs_stats.total_entries,
             nvs_stats.namespace_count);

    nvs_iterator_t it = NULL;

    esp_err_t res = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);

    while (res == ESP_OK)
    {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        ESP_LOGI(TAG_LOCAL,
                 "NVS: namespace '%s' : key '%s', type '%d'",
                 info.namespace_name,
                 info.key,
                 info.type);

        res = nvs_entry_next(&it);
    }

    nvs_release_iterator(it);
}

// MARK: app_main
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Wi-Fi Diagnostic Logger started");

    esp_err_t ret = nvs_flash_init();

    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (ret == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    nvs_list_all();

    wifi_provision_care(TAG);

    ESP_LOGI(TAG, "Publish mDNS hostname %s.local.", TAG);

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(TAG));

    while (true)
    {
        ESP_LOGI(TAG,
                 "Minimum free heap size: %" PRIu32 " bytes",
                 esp_get_minimum_free_heap_size());

        esp_netif_t *sta_netif = wifi_provision_care_get_sta_netif();

        if (sta_netif != NULL)
        {
            esp_netif_ip_info_t ip_info;

            if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK)
            {
                ESP_LOGI(TAG, "- STA IPv4 address: " IPSTR, IP2STR(&ip_info.ip));
            }
            else
            {
                ESP_LOGW(TAG, "Failed to get STA IPv4 info");
            }

            esp_ip6_addr_t ip6[5];
            int ip6_addrs = esp_netif_get_all_ip6(sta_netif, ip6);

            for (int j = 0; j < ip6_addrs; ++j)
            {
                esp_ip6_addr_type_t ipv6_type =
                    esp_netif_ip6_get_addr_type(&(ip6[j]));

                ESP_LOGI(TAG,
                         "- IPv6 address: " IPV6STR ", type: %d",
                         IPV62STR(ip6[j]),
                         ipv6_type);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Default netif is NULL");
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}