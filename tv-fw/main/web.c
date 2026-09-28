// Servidor web de BIOMA (por la red USB): http://192.168.7.1
//   /                 página de control      /api/estado   genoma, generación, uso del chip (JSON)
//   /api/dia?v=144    velocidad del día      /api/hora?s=  hora real (segundos desde medianoche)
//   /snap.jpg         foto 1080p actual      :81/stream    video MJPEG en vivo
//   /api/evo?modo=0   0 = juez JPEG · 1 = azar · 2 = congelado (las 3 condiciones del experimento del paper)
//   y los de sistema.c: /api/log /api/tasks /api/particiones /api/flash /api/volver /ota (protege a Cerebro P4)
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "sistema.h"
#include "tv.h"
#include "wifi.h"
#include "lcd.h"
#include "tierra_app.h"

static const char *TAG = "web";

extern const char page_start[] asm("_binary_bioma_html_start");
extern const char page_end[]   asm("_binary_bioma_html_end");

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, page_start, page_end - page_start);
}

static esp_err_t h_estado(httpd_req_t *r)
{
    char b[1024];
    bioma_status(b, sizeof(b));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    ota_mark_ok_if_pending();          // el PC logró hablarle: el firmware sirve
    return httpd_resp_sendstr(r, b);
}

static esp_err_t h_dia(httpd_req_t *r)
{
    char q[64], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "v", v, sizeof(v)) == ESP_OK) {
        lpcore_set_speed((uint32_t)atoi(v));
    }
    return h_estado(r);
}

static esp_err_t h_hora(httpd_req_t *r)
{
    char q[64], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK) {
        lpcore_set_time((uint32_t)atoi(v));
        bioma_set_time((uint32_t)atoi(v));
    }
    return h_estado(r);
}

static esp_err_t h_reloj(httpd_req_t *r)
{
    char q[32], v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "v", v, sizeof(v)) == ESP_OK) {
        bioma_set_clock(atoi(v) != 0);
    }
    return h_estado(r);
}

static esp_err_t h_evo(httpd_req_t *r)
{
    char q[64], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "modo", v, sizeof(v)) == ESP_OK) {
        bioma_set_evo_mode(atoi(v));
    }
    return h_estado(r);
}

static esp_err_t h_snap(httpd_req_t *r)
{
    // ?chico=1: la foto liviana de 480x264 (~20 KB), la que usa la página cuando se mira por Wi-Fi
    char q[48], v[8];
    bool chico = httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "chico", v, sizeof(v)) == ESP_OK && v[0] == '1';
    size_t cap = chico ? 128 * 1024 : 768 * 1024;
    uint8_t *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    uint32_t seq = 0;
    size_t n = !b ? 0 : chico ? bioma_copy_jpeg_chico(b, cap, &seq, pdMS_TO_TICKS(3000)) : bioma_copy_jpeg(b, cap, &seq, pdMS_TO_TICKS(2000));
    httpd_resp_set_type(r, "image/jpeg");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = n ? httpd_resp_send(r, (const char *)b, n) : httpd_resp_send_500(r);
    free(b);
    return e;
}

// ---------------- Video en vivo (puerto 81, una tarea por espectador) ----------------
#define BOUNDARY "biomap4frame"

static void stream_task(void *arg)
{
    httpd_req_t *r = arg;
    // ?chico=1: video liviano de 480x264 (para mirar por Wi-Fi a través del C3)
    char q[32], v[8];
    bool chico = httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "chico", v, sizeof(v)) == ESP_OK && v[0] == '1';
    size_t cap = 768 * 1024;
    uint8_t *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    httpd_resp_set_type(r, "multipart/x-mixed-replace; boundary=" BOUNDARY);
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    uint32_t seq = 0;
    char hdr[96];
    while (b) {
        size_t n = chico ? bioma_copy_jpeg_chico(b, cap, &seq, pdMS_TO_TICKS(3000)) : bioma_copy_jpeg(b, cap, &seq, pdMS_TO_TICKS(3000));
        if (!n) continue;
        int hl = snprintf(hdr, sizeof(hdr), "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)n);
        if (httpd_resp_send_chunk(r, hdr, hl) != ESP_OK || httpd_resp_send_chunk(r, (const char *)b, n) != ESP_OK) {
            break;
        }
    }
    free(b);
    httpd_req_async_handler_complete(r);
    vTaskDelete(NULL);
}

static esp_err_t h_stream(httpd_req_t *r)
{
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(r, &copy) != ESP_OK) return ESP_FAIL;
    // prioridad sobre la simulación (8-9): con los 2 núcleos al 100 %, a prioridad 6 casi no le tocaba turno y la página
    // recibía < 1 cuadro por segundo (medido). Mandar un cuadro toma pocos ms y el resto del tiempo espera.
    if (xTaskCreatePinnedToCore(stream_task, "stream", 6144, copy, 10, NULL, 0) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t h_escena(httpd_req_t *r)
{
    char q[32], v[4];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "e", v, sizeof(v)) == ESP_OK)
        bioma_set_escena(atoi(v));
    char t[400], b[460];
    tierra_estado_json(t, sizeof(t));
    snprintf(b, sizeof(b), "{\"escena\":\"%s\",\"tierra\":%s}", bioma_escena() ? "tierra" : "bioma", t);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, b);
}

void web_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = PORT_WEB;
    c.ctrl_port = 32768;
    c.max_uri_handlers = 32;
    c.stack_size = 7168;            // la autoprueba de la tele (PARLIO + comparar) corre dentro del servidor
    c.core_id = 0;
    c.lru_purge_enable = true;
    c.task_priority = 12;          // la web nunca se queda sin turno aunque la simulación vaya a full
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &c) == ESP_OK) {
        httpd_uri_t u[] = {
            { .uri = "/", .method = HTTP_GET, .handler = h_root },
            { .uri = "/api/estado", .method = HTTP_GET, .handler = h_estado },
            { .uri = "/api/dia", .method = HTTP_GET, .handler = h_dia },
            { .uri = "/api/hora", .method = HTTP_GET, .handler = h_hora },
            { .uri = "/api/reloj", .method = HTTP_GET, .handler = h_reloj },
            { .uri = "/api/evo", .method = HTTP_GET, .handler = h_evo },
            { .uri = "/snap.jpg", .method = HTTP_GET, .handler = h_snap },
        };
        for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(s, &u[i]);
        sistema_registrar(s);          // /api/log /api/tasks /api/particiones /api/flash /api/volver /ota (protege a Cerebro)
        tv_registrar(s);               // /api/tv /api/tv/prueba /api/tv/captura.bin /api/tv/cuadro.bin
        wifi_registrar(s);             // /api/wifi
        lcd_registrar(s);              // /api/pantalla
        httpd_uri_t ue = { .uri = "/api/escena", .method = HTTP_GET, .handler = h_escena };
        httpd_register_uri_handler(s, &ue);   // ?e=0 BIOMA · ?e=1 TIERRA VIVA (y el estado de la red y los datos)
    }

    httpd_config_t c2 = HTTPD_DEFAULT_CONFIG();
    c2.server_port = PORT_STREAM;
    c2.ctrl_port = 32769;
    c2.max_open_sockets = 3;
    c2.core_id = 0;
    c2.lru_purge_enable = true;
    c2.task_priority = 7;
    httpd_handle_t s2 = NULL;
    if (httpd_start(&s2, &c2) == ESP_OK) {
        httpd_uri_t us = { .uri = "/stream", .method = HTTP_GET, .handler = h_stream };
        httpd_register_uri_handler(s2, &us);
    }
    ESP_LOGI(TAG, "BIOMA en http://" P4_IP_STR "  (video en :81/stream)");
}
