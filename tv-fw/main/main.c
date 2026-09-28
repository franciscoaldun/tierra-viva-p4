// BIOMA — fondo de pantalla vivo en el ESP32-P4 (WT9932P4-TINY, chip v1.0, 32 MB PSRAM).
// Un cable USB al PC: webcam UVC 1080p con la colonia viva + red NCM con página de control en http://192.168.7.1
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "sistema.h"
#include "tv.h"
#include "wifi.h"
#include "lcd.h"

// 0 = firmware sólo con Wi-Fi (para aislar una caída al arrancar); 1 = con la salida de TV
#ifndef ARRANCAR_TELE
#define ARRANCAR_TELE 0
#endif

static void mem_log(const char *paso)
{
    ESP_LOGI("mem", "%-8s RAM interna libre %3u KB (bloque mayor %3u KB) | PSRAM libre %5u KB", paso,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void app_main(void)
{
    sistema_early_init();       // antes que nada: registro en memoria + guardián de vuelta atrás (protege a Cerebro)
    mem_log("inicio");
    bioma_early_init();         // el rastro necesita RAM interna en bloques grandes
    mem_log("rastro");
    // la NVS se comparte con Cerebro P4: si da error, NO se borra (BIOMA no la necesita)
    esp_err_t r = nvs_flash_init();
    if (r != ESP_OK) ESP_LOGW("main", "NVS no disponible (%s): sigo sin ella", esp_err_to_name(r));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    ESP_LOGI("main", "BIOMA: chip rev v%d.%d, %d núcleos", ci.revision / 100, ci.revision % 100, ci.cores);

    stats_start();
    lpcore_start();
    usb_start();
    mem_log("usb");
    bioma_start();
    mem_log("bioma");
    if (ARRANCAR_TELE) tv_start();   // video compuesto por IO16-21 y sonido por IO22 (aunque no haya tele conectada)
    mem_log("tele");
    lcd_start();                // pantalla táctil de 2,8" en los 14 primeros pines del header derecho
    mem_log("pantalla");
    web_start();
    mem_log("web");
    wifi_start();               // Wi-Fi por el C3 montado encima: sólo si hay una red guardada
    mem_log("wifi");
}
