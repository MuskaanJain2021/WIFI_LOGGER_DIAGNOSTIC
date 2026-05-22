idf_component_register(
    SRCS 
        "main.c"
    INCLUDE_DIRS 
        "."
    PRIV_REQUIRES 
        esp_wifi 
        esp_event
        nvs_flash
        espressif__mdns                # Added: Pulls in mdns.h configuration hooks
        Wifi_Provisioning_Care_ESP32
)