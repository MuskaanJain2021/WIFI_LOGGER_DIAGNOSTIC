# 1. Register the component with correct paths matching your new folder layout
idf_component_register(
    SRCS
        "ESP32_WIFI_Provisioning_Care.c"
        "dns_server.c"
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        "web_assets/esp32_wifi_provision_care_favicon.ico" # Fixed: Pointing inside web_assets
        "${COMPONENT_DIR}/web_assets/wifi.html.gz"         # Fixed: Output directory targeted correctly
    PRIV_REQUIRES
        esp_http_server
        esp_wifi
        nvs_flash
        json
        app_update     
)

# 2. Hide the build automation blocks from the initial requirements pass
if(NOT __IDF_SERVICES_LOOKUP)

    # Updated: Targets the new web_assets directory for input and output
    add_custom_command(
        OUTPUT
            "${COMPONENT_DIR}/web_assets/wifi.html.gz"
        WORKING_DIRECTORY
            "${COMPONENT_DIR}/web_assets"
        COMMAND
            ${CMAKE_COMMAND} -E echo "Gzipping wifi.html..."
        COMMAND
            python3 -c "import gzip,shutil;f_in=open('wifi.html','rb');f_out=gzip.open('wifi.html.gz','wb');shutil.copyfileobj(f_in,f_out);f_in.close();f_out.close()"
        DEPENDS
            "${COMPONENT_DIR}/web_assets/wifi.html"
        VERBATIM
    )

    # Tracking target configuration
    add_custom_target(
        generate_wifi_html_gz
        DEPENDS "${COMPONENT_DIR}/web_assets/wifi.html.gz"
    )

    add_dependencies(${COMPONENT_LIB} generate_wifi_html_gz)

endif()