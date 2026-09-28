// Sistema común de los firmwares del laboratorio (NEÓN, BIOMA): lo que protege a Cerebro P4 y deja depurar sin cable serie.
//   - Guardián de vuelta atrás: arranca en la primera línea de app_main. Si el firmware nuevo no logra hablar con el
//     PC en 3 minutos, el bootloader vuelve al anterior (Cerebro).
//   - /ota se NIEGA a escribir encima de Cerebro P4 (con sólo 2 espacios, el otro espacio es el de Cerebro).
//   - /api/volver: reinicia en el firmware del otro espacio sin reescribirlo.
//   - /api/flash?ini=&len=: lee la flash cruda (respaldo completo por la red).
//   - /api/particiones: tabla de particiones, qué hay en cada espacio y su estado.
//   - /api/log: el registro en memoria (no hay consola serie: sólo está conectado el USB de la webcam).
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sistema.h"
#include "tusb.h"

// motivo del reinicio anterior: si el firmware anterior se cayó al arrancar, aquí queda dicho por qué
static esp_reset_reason_t s_rr;
static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "encendido";
    case ESP_RST_SW: return "reinicio por software";
    case ESP_RST_PANIC: return "PANICO (excepcion)";
    case ESP_RST_INT_WDT: return "perro guardian de interrupciones";
    case ESP_RST_TASK_WDT: return "perro guardian de tareas";
    case ESP_RST_WDT: return "otro perro guardian";
    case ESP_RST_BROWNOUT: return "baja de voltaje";
    case ESP_RST_DEEPSLEEP: return "salida de sueño profundo";
    default: return "otro";
    }
}

// antes de reiniciar: desconectar el USB con calma para que Windows vea una salida ordenada
// (sospecha: reiniciar de golpe con el USB hablando causa los arranques fallidos intermitentes)
static void reiniciar_limpio(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));      // que alcance a salir la respuesta HTTP
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static const char *TAG = "sistema";

// ---------------- registro en memoria ----------------
#define LOG_RING 16384
static char s_log[LOG_RING];
static size_t s_log_head;
static bool s_log_wrap;
static vprintf_like_t s_prev_vprintf;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static int log_vprintf(const char *fmt, va_list ap)
{
    char line[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
        portENTER_CRITICAL(&s_log_mux);
        for (int i = 0; i < n; i++) {
            s_log[s_log_head++] = line[i];
            if (s_log_head == LOG_RING) { s_log_head = 0; s_log_wrap = true; }
        }
        portEXIT_CRITICAL(&s_log_mux);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : n;
}

// ---------------- guardián de vuelta atrás ----------------
static volatile bool s_pending;

void ota_mark_ok_if_pending(void)
{
    if (s_pending) {
        s_pending = false;
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware nuevo confirmado (el PC le habló)");
    }
}

static void ota_guard_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(180000));
    if (s_pending) {
        ESP_LOGE(TAG, "el firmware nuevo no logró conectarse en 3 minutos: vuelvo al anterior");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelete(NULL);
}

void sistema_early_init(void)
{
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
    s_rr = esp_reset_reason();
    ESP_LOGW(TAG, "reinicio anterior: %s", reset_name(s_rr));
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        xTaskCreatePinnedToCore(ota_guard_task, "ota_guard", 3072, NULL, 5, NULL, 0);
        ESP_LOGW(TAG, "firmware nuevo a prueba: se confirma cuando el PC le hable (si no, en 3 min vuelve al anterior)");
    }
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (%s %s) en %s @0x%lx", d->project_name, d->version, d->date, d->time, run->label,
             (unsigned long)run->address);
}

// ---------------- manejadores HTTP ----------------
static esp_err_t h_log(httpd_req_t *r)
{
    char *b = heap_caps_malloc(LOG_RING + 1, MALLOC_CAP_SPIRAM);
    if (!b) return httpd_resp_send_500(r);
    int n = 0;
    portENTER_CRITICAL(&s_log_mux);
    if (s_log_wrap) for (size_t i = s_log_head; i < LOG_RING; i++) b[n++] = s_log[i];
    for (size_t i = 0; i < s_log_head; i++) b[n++] = s_log[i];
    portEXIT_CRITICAL(&s_log_mux);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    esp_err_t e = httpd_resp_send(r, b, n);
    free(b);
    return e;
}

static esp_err_t h_tasks(httpd_req_t *r)
{
    char *b = malloc(4096);
    if (!b) return httpd_resp_send_500(r);
    vTaskGetRunTimeStats(b);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    esp_err_t e = httpd_resp_sendstr(r, b);
    free(b);
    return e;
}

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "nuevo";
    case ESP_OTA_IMG_PENDING_VERIFY: return "a prueba";
    case ESP_OTA_IMG_VALID: return "valido";
    case ESP_OTA_IMG_INVALID: return "invalido";
    case ESP_OTA_IMG_ABORTED: return "abortado";
    default: return "sin marca";
    }
}

static esp_err_t h_particiones(httpd_req_t *r)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    char *b = malloc(4096);
    if (!b) return httpd_resp_send_500(r);
    int n = snprintf(b, 4096, "{\"corriendo\":\"%s\",\"arranque\":\"%s\",\"reinicio_anterior\":\"%s\",\"particiones\":[",
                     run->label, boot ? boot->label : "?", reset_name(s_rr));
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool first = true;
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        n += snprintf(b + n, 4096 - n, "%s{\"nombre\":\"%s\",\"tipo\":%d,\"subtipo\":%d,\"dir\":\"0x%lx\",\"tam\":\"0x%lx\"",
                      first ? "" : ",", p->label, p->type, p->subtype, (unsigned long)p->address, (unsigned long)p->size);
        first = false;
        if (p->type == ESP_PARTITION_TYPE_APP) {
            esp_app_desc_t d;
            esp_ota_img_states_t st;
            if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
                char sha[17];
                for (int i = 0; i < 8; i++) sprintf(sha + 2 * i, "%02x", d.app_elf_sha256[i]);
                n += snprintf(b + n, 4096 - n, ",\"app\":\"%s\",\"version\":\"%s\",\"fecha\":\"%s %s\",\"elf_sha256\":\"%s\"",
                              d.project_name, d.version, d.date, d.time, sha);
            } else {
                n += snprintf(b + n, 4096 - n, ",\"app\":null");
            }
            if (esp_ota_get_state_partition(p, &st) == ESP_OK) n += snprintf(b + n, 4096 - n, ",\"estado\":\"%s\"", state_name(st));
        }
        n += snprintf(b + n, 4096 - n, "}");
    }
    esp_partition_iterator_release(it);
    n += snprintf(b + n, 4096 - n, "]}");
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_send(r, b, n);
    free(b);
    return e;
}

// lectura cruda de la flash: /api/flash?ini=0&len=16777216 (respaldo completo por la red)
static esp_err_t h_flash(httpd_req_t *r)
{
    char q[64], v[24];
    uint32_t ini = 0, len = 0x1000000;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "ini", v, sizeof(v)) == ESP_OK) ini = strtoul(v, NULL, 0);
        if (httpd_query_key_value(q, "len", v, sizeof(v)) == ESP_OK) len = strtoul(v, NULL, 0);
    }
    uint32_t chip_size = 0;
    esp_flash_get_size(NULL, &chip_size);
    if (ini >= chip_size) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "fuera de la flash");
    if (len > chip_size - ini) len = chip_size - ini;
    const size_t CH = 8192;
    uint8_t *buf = heap_caps_malloc(CH, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);   // RAM interna: la flash se lee con la caché apagada
    if (!buf) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/octet-stream");
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"flash_0x%lx_0x%lx.bin\"", (unsigned long)ini, (unsigned long)len);
    httpd_resp_set_hdr(r, "Content-Disposition", disp);
    ESP_LOGI(TAG, "leyendo flash 0x%lx + 0x%lx", (unsigned long)ini, (unsigned long)len);
    esp_err_t e = ESP_OK;
    for (uint32_t off = 0; off < len && e == ESP_OK; off += CH) {
        uint32_t n = len - off < CH ? len - off : CH;
        e = esp_flash_read(NULL, buf, ini + off, n);
        if (e == ESP_OK) e = httpd_resp_send_chunk(r, (const char *)buf, n);
    }
    free(buf);
    if (e == ESP_OK) httpd_resp_send_chunk(r, NULL, 0);
    return e;
}

static esp_err_t h_volver(httpd_req_t *r)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "No hay otro firmware valido en el otro espacio OTA.\n");
    }
    if (esp_ota_set_boot_partition(other) != ESP_OK) return httpd_resp_send_500(r);   // verifica la imagen antes
    char msg[200];
    snprintf(msg, sizeof(msg), "OK: reiniciando en \"%s\" (%s). Este firmware queda guardado en su espacio.\n", d.project_name, d.version);
    httpd_resp_sendstr(r, msg);
    ESP_LOGW(TAG, "volviendo a %s", d.project_name);
    reiniciar_limpio();
    return ESP_OK;
}

static esp_err_t h_ota(httpd_req_t *r)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    char q[32] = "";
    httpd_req_get_url_query_str(r, q, sizeof(q));
    esp_app_desc_t d;
    // con 2 espacios, el otro espacio es el de Cerebro P4: no se escribe encima salvo que se pida a propósito
    if (part && !strstr(q, "forzar=1") && esp_ota_get_partition_description(part, &d) == ESP_OK &&
        strcmp(d.project_name, "p4_webcam") == 0) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "El otro espacio tiene Cerebro P4 y no se toca. Vuelve a Cerebro (/api/volver) y sube el firmware desde ahi.\n");
    }
    esp_ota_handle_t h;
    if (!part || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) return httpd_resp_send_500(r);
    char *buf = malloc(16384);
    if (!buf) {
        esp_ota_abort(h);
        return httpd_resp_send_500(r);
    }
    int left = r->content_len, got = 0;
    ESP_LOGI(TAG, "OTA: recibiendo %d KB en %s", left / 1024, part->label);
    while (left > 0) {
        int n = httpd_req_recv(r, buf, left > 16384 ? 16384 : left);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || esp_ota_write(h, buf, n) != ESP_OK) {
            free(buf);
            esp_ota_abort(h);
            return httpd_resp_send_500(r);
        }
        left -= n;
        got += n;
    }
    free(buf);
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) return httpd_resp_send_500(r);
    httpd_resp_sendstr(r, "OK, reiniciando con el firmware nuevo\n");
    ESP_LOGI(TAG, "OTA OK (%d KB), reiniciando", got / 1024);
    reiniciar_limpio();
    return ESP_OK;
}

// /api/reiniciar: reinicio ordenado del mismo firmware (para cuando se maneja sólo por Wi-Fi)
static esp_err_t h_reiniciar(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "OK, reiniciando el P4 (vuelve en ~15 s)\n");
    ESP_LOGW(TAG, "reinicio pedido desde la web");
    reiniciar_limpio();
    return ESP_OK;
}

void sistema_registrar(httpd_handle_t s)
{
    httpd_uri_t u[] = {
        { .uri = "/api/log", .method = HTTP_GET, .handler = h_log },
        { .uri = "/api/tasks", .method = HTTP_GET, .handler = h_tasks },
        { .uri = "/api/particiones", .method = HTTP_GET, .handler = h_particiones },
        { .uri = "/api/flash", .method = HTTP_GET, .handler = h_flash },
        { .uri = "/api/volver", .method = HTTP_GET, .handler = h_volver },
        { .uri = "/ota", .method = HTTP_POST, .handler = h_ota },
        { .uri = "/api/reiniciar", .method = HTTP_GET, .handler = h_reiniciar },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(s, &u[i]);
}
