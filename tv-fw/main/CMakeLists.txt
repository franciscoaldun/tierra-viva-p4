idf_component_register(
    SRCS "main.c" "usb.c" "bioma.c" "bioma_sim.c" "bioma_evo.c" "bioma_reloj.c" "stats.c" "lpcore.c" "web.c" "sistema.c"
         "ntsc_core.c" "tv.c" "tv_audio.c" "wifi.c" "lcd.c" "tierra.c" "tierra_app.c" "red.c"
    INCLUDE_DIRS "."
    REQUIRES esp_tinyusb tinyusb esp_netif esp_event lwip esp_http_server app_update nvs_flash
             esp_driver_jpeg esp_driver_ppa esp_driver_tsens esp_timer esp_mm esp_psram ulp spi_flash esp_partition
             esp_hal_lcd esp_driver_dma esp_driver_gpio esp_driver_parlio esp_driver_pcnt esp_driver_i2s
             esp_wifi esp_wifi_remote esp_hosted esp_lcd esp_driver_spi esp_driver_ledc mbedtls
    EMBED_TXTFILES "bioma.html"
    EMBED_FILES "../../tierra/dia565.bin" "../../tierra/luces8.bin" "../../tierra/mar8.bin" "../../tierra/nubes8.bin")

# Programa del núcleo LP (ulp/main.c): el "reloj de reposo" que lleva la hora del día
set(ulp_app_name lp_core_main)
set(ulp_lp_core_sources "ulp/main.c")
set(ulp_exp_dep_srcs "lpcore.c")
ulp_embed_binary(${ulp_app_name} "${ulp_lp_core_sources}" "${ulp_exp_dep_srcs}")
