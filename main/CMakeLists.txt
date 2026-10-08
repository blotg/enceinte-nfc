idf_component_register(
    SRCS
        "main.c"
        "settings.c"
        "storage.c"
        "cards.c"
        "pn532.c"
        "pn532_frame.c"
        "nfc.c"
        "player.c"
        "media_info.c"
        "controller.c"
        "session.c"
        "resume_store.c"
        "wifi_mgr.c"
        "dns_server.c"
        "web_server.c"
        "tls_cert.c"
        "mpd_server.c"
        "mpd_proto.c"
        "ota.c"
        "changes.c"
        "util.c"
        "dsp.c"
        "buttons.c"
        "config_json.c"
        "backup.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "web/index.html"
        "web/app.js"
        "web/style.css"
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client
        esp_https_ota app_update fatfs sdmmc esp_driver_sdspi esp_driver_spi
        esp_driver_i2s esp_driver_uart esp_driver_gpio esp_timer json mbedtls
        esp_partition lwip esp_https_server
)
