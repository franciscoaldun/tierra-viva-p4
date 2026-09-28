// BIOMA — la colonia de vida artificial (parte del P4: tareas, aceleradores y evolución).
//   Núcleos 0 y 1: agentes Physarum (en PSRAM, recorridos en orden) + difusión del rastro (RAM interna)
//   → coloreado a 480x270 con la paleta del "sol" que lleva el núcleo LP
//   → PPA escala ×4 a 1920x1080 SIN bloquear (mientras tanto los núcleos simulan el cuadro siguiente)
//   → tarea aparte: codificador JPEG por hardware → webcam UVC y :81/stream
//   → el juez: 1 de cada 10 cuadros se codifica ADEMÁS como JPEG en gris de 480x270 (sin paleta, ~1,5 ms);
//     su tamaño es la complejidad. El genoma muta y sobrevive lo que se acerca al objetivo.
// La simulación en sí está en bioma_sim.c (C puro, verificada en el PC con host_test/).
//
// Memoria: el mapa de rastro (480x270x2 = 253 KB) no cabe en el bloque interno más grande del P4 v1.0
// (248 KB al arrancar, medido en Cerebro P4), así que se reserva en trozos de filas con una tabla.
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/ppa.h"
#include "esp_async_memcpy.h"
#include "driver/jpeg_encode.h"
#include "app.h"
#include "bioma_evo.h"
#include "bioma_reloj.h"
#include "tv.h"
#include "lcd.h"
#include "tierra_app.h"
#include "nvs.h"

static const char *TAG = "bioma";

#define OW 1920
#define OH 1080
#define OH16 1088                        // alto rellenado a múltiplo de 16 (lo pide el JPEG)
#define N_AGENTS 200000
#define STEPS_PER_FRAME 1                // medido: 2 pasos daban 7 fps; con 1 la vida avanza casi igual y el video sale más fluido
#define N_FRAMES 2                       // cuadros 1080p: uno lo escribe el PPA, el otro lo lee el JPEG

static agent_t  *s_ag, *s_ag_tmp;         // PSRAM, 1,2 MB cada uno (el segundo, para ordenarlos por fila)
#define SORT_EVERY 0                     // 0 = apagado. Medido en el P4: ordenar por fila NO aceleró el paso (54-58 ms igual)
static float s_t_sort;
// reloj: la hora real llega del PC (/api/hora, la página la manda al abrirse) y se sigue con el temporizador del chip
static volatile int32_t s_hora_base = -1;          // segundos desde medianoche al sincronizar (-1 = sin hora)
static volatile int64_t s_hora_us;
static volatile bool s_reloj_on = true;
void bioma_set_time(uint32_t sec) { s_hora_us = esp_timer_get_time(); s_hora_base = (int32_t)(sec % 86400); }
void bioma_set_clock(bool on) { s_reloj_on = on; }
static int64_t hora_ms(void)                        // milisegundos desde medianoche (-1 = sin hora)
{
    if (s_hora_base < 0) return -1;
    return ((int64_t)s_hora_base * 1000 + (esp_timer_get_time() - s_hora_us) / 1000) % 86400000;
}
static int hora_segundos(void) { int64_t m = hora_ms(); return m < 0 ? -1 : (int)(m / 1000); }
static uint32_t s_seed_state;
static uint32_t fast_rnd(void) { uint32_t x = s_seed_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s_seed_state = x; }
static uint8_t  *s_small;                 // 480x270 RGB565 (entrada del PPA), PSRAM
static size_t    s_small_cap;
static uint8_t  *s_small_a;               // la imagen chica original (TIERRA alterna entre ésta y s_small2)
static uint8_t  *s_frame[N_FRAMES];       // 1920x1088 RGB565 (salida del PPA / entrada del JPEG), PSRAM
static size_t    s_frame_cap;
static uint8_t  *s_gray;                  // 480x272 en gris: lo que mira el juez (sin paleta), PSRAM
static size_t    s_gray_cap;
static uint8_t  *s_gray_jpeg;
static size_t    s_gray_jpeg_cap;
static volatile bool s_gray_ready;        // bioma_task lo llena, enc_task lo codifica y se lo pasa al juez
#define JUDGE_EVERY 10                    // el juez mira 1 de cada 10 cuadros (~1,5 ms de JPEG cada vez)
// video liviano para mirar por Wi-Fi (el enlace con el C3 da ~90 KB/s: un 1080p de ~300 KB tardaría segundos)
#define CH_H 256                          // 480x256: alto múltiplo de 16 para el JPEG 4:2:0 (se recortan 14 filas)
static uint8_t  *s_chico, *s_jchico;      // copia de la imagen chica y su JPEG (PSRAM)
static size_t    s_chico_cap, s_jchico_cap, s_jchico_len;
static volatile bool s_chico_listo;       // bioma_task copió un cuadro: enc_task lo codifica
static volatile uint32_t s_jchico_seq;
static volatile int64_t s_chico_pedido_us;   // último espectador por Wi-Fi (se codifica sólo si alguien mira)
static uint8_t  *s_jpeg[2];
static size_t    s_jpeg_cap, s_jpeg_len[2];
static int       s_jlatest = -1, s_jusb = -1;
static volatile uint32_t s_jseq;
static SemaphoreHandle_t s_jmux, s_ppa_done, s_free_frames;
static QueueHandle_t     s_enc_q;
static jpeg_encoder_handle_t s_jenc;
static ppa_client_handle_t   s_srm;
static bool s_422;

// ---------------- genoma (la evolución está en bioma_evo.c, C puro) ----------------
static portMUX_TYPE s_gmux = portMUX_INITIALIZER_UNLOCKED;
static evo_t    s_evo;
static float s_t_agents, s_t_diffuse, s_t_color, s_t_wait_ppa, s_t_wait_buf, s_t_frame;   // ms por cuadro (medidos)
static genome_t s_cur;                    // el que usan los núcleos en este paso (se copia antes de empezar)
// renacimiento 1 %: host_test mostró que sin renacimiento la red colapsa en un solo anillo (JPEG 50 KB)
static const genome_t G0 = { .sa = 0.77f, .ra = 0.53f, .sd = 12.0f, .ss = 1.0f, .decay = 0.86f, .dep = 2.2f,
                             .respawn = 0.01f, .palette = 0 };

static genome_t genome_now(void)
{
    portENTER_CRITICAL(&s_gmux);
    genome_t g = *evo_current(&s_evo);
    portEXIT_CRITICAL(&s_gmux);
    return g;
}

void bioma_set_evo_mode(int m)
{
    portENTER_CRITICAL(&s_gmux);
    s_evo.mode = m == 1 ? EVO_RANDOM : m == 2 ? EVO_FROZEN : EVO_JPEG;
    if (s_evo.mode == EVO_FROZEN) s_evo.phase = 0;
    portEXIT_CRITICAL(&s_gmux);
}

// ---------------- trabajo repartido en los 2 núcleos ----------------
typedef enum { JOB_AGENTS, JOB_DIFFUSE, JOB_COLOR, JOB_TIERRA } job_t;
static const tierra_t *s_tierra;           // el cuadro de TIERRA VIVA que dibujan los 2 núcleos
static uint8_t *s_small2, *s_dibujo;       // TIERRA: se dibuja en una imagen mientras el PPA escala la otra
typedef struct {
    int core, a_first, a_last;
    job_t job;
    uint32_t hist[64];
    SemaphoreHandle_t go, done;
} worker_t;
static worker_t s_wk[2];

// TIERRA: cada núcleo dibuja 3 filas en una baldosa de RAM interna y el DMA la copia a la PSRAM.
// Escribir con la CPU directo en la PSRAM es lo más lento del P4 (medido en NEÓN: 82 -> 40 ms con baldosas).
#define TT_FILAS 3
static uint16_t *s_tt[2];
static async_memcpy_handle_t s_tdma[2];
static SemaphoreHandle_t s_tlista[2];
static bool on_tdma(async_memcpy_handle_t h, async_memcpy_event_t *e, void *arg)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &hp);
    return hp == pdTRUE;
}

static void baldosas_tierra(void)            // la primera vez que se ve el globo (ya con la pantalla y la web en pie)
{
    static bool probado;
    if (probado) return;
    probado = true;
    // APAGADO: medido, no aceleró el globo (53 vs 50 ms) y sus 2 canales de DMA dejaban al cifrado por hardware
    // sin canal ("crypto_shared_gdma: Failed to acquire DMA channel") -> el HTTPS fallaba siempre.
    if (1) return;
    for (int c = 0; c < 2; c++) {
        async_memcpy_config_t mc = ASYNC_MEMCPY_DEFAULT_CONFIG();
        mc.backlog = 2;
        mc.dma_burst_size = 64;
        s_tt[c] = heap_caps_aligned_calloc(64, 1, TW * TT_FILAS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_tlista[c] = xSemaphoreCreateBinary();
        if (!s_tt[c] || !s_tlista[c] || esp_async_memcpy_install_gdma_axi(&mc, &s_tdma[c]) != ESP_OK) {
            ESP_LOGW(TAG, "TIERRA sin baldosas (RAM interna o DMA): se dibuja directo en la PSRAM");
            s_tt[0] = s_tt[1] = NULL;
            return;
        }
    }
    ESP_LOGI(TAG, "TIERRA: baldosas de %d filas en RAM interna + 2 canales de DMA", TT_FILAS);
}

static float s_tt_ms[2];
static void pintar_tierra(int core)
{
    int64_t tq = esp_timer_get_time();
    int y0 = core ? TH / 2 : 0, y1 = core ? TH : TH / 2;
    uint16_t *dst = (uint16_t *)s_dibujo, *tile = s_tt[core];
    if (!tile) { tierra_filas(s_tierra, dst + y0 * TW, TW, TH, y0, y1); return; }
    for (int y = y0; y < y1; y += TT_FILAS) {
        int n = y + TT_FILAS <= y1 ? TT_FILAS : y1 - y;
        tierra_filas(s_tierra, tile, TW, TH, y, y + n);
        size_t b = (size_t)n * TW * 2;
        esp_cache_msync(tile, (b + 63) & ~63, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (esp_async_memcpy(s_tdma[core], dst + y * TW, tile, b, on_tdma, s_tlista[core]) == ESP_OK)
            xSemaphoreTake(s_tlista[core], portMAX_DELAY);   // 2,9 KB: el DMA tarda microsegundos
        else memcpy(dst + y * TW, tile, b);
    }
    s_tt_ms[core] = (esp_timer_get_time() - tq) / 1000.0f;
}

static void worker_task(void *arg)
{
    worker_t *w = arg;
    while (1) {
        xSemaphoreTake(w->go, portMAX_DELAY);
        if (w->job == JOB_AGENTS)       sim_agents_step(s_ag, w->a_first, w->a_last, &s_cur, esp_random());
        else if (w->job == JOB_DIFFUSE) sim_diffuse_half(w->core, s_cur.decay, w->hist);
        else if (w->job == JOB_TIERRA)  pintar_tierra(w->core);
        else                            sim_colorize_rows((uint16_t *)s_small, s_wk[0].hist, s_wk[1].hist,
                                                          w->core ? TH / 2 : 0, w->core ? TH : TH / 2);
        xSemaphoreGive(w->done);
    }
}

static void run_job(job_t j)
{
    if (j == JOB_DIFFUSE) sim_snapshot_edges();    // fotos de las filas de borde ANTES de que alguien escriba
    s_wk[0].job = s_wk[1].job = j;
    xSemaphoreGive(s_wk[0].go);
    xSemaphoreGive(s_wk[1].go);
    xSemaphoreTake(s_wk[0].done, portMAX_DELAY);
    xSemaphoreTake(s_wk[1].done, portMAX_DELAY);
}

// ---------------- PPA ×4 sin bloquear ----------------
static volatile int64_t s_ppa_t0, s_ppa_t1;
static bool on_ppa_done(ppa_client_handle_t c, ppa_event_data_t *e, void *u)
{
    BaseType_t hp = pdFALSE;
    s_ppa_t1 = esp_timer_get_time();                 // hora real en que terminó (medido en NEÓN: si no, se infla)
    xSemaphoreGiveFromISR(s_ppa_done, &hp);
    return hp == pdTRUE;
}

static bool upscale_start(int k)
{
    ppa_srm_oper_config_t op = {
        .in = { .buffer = s_small, .pic_w = TW, .pic_h = TH, .block_w = TW, .block_h = TH, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = s_frame[k], .buffer_size = s_frame_cap, .pic_w = OW, .pic_h = OH16, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 4.0f, .scale_y = 4.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    s_ppa_t0 = esp_timer_get_time();
    return ppa_do_scale_rotate_mirror(s_srm, &op) == ESP_OK;
}

// ---------------- codificador (tarea aparte, núcleo 1) ----------------
static void enc_task(void *arg)
{
    int k;
    while (1) {
        xQueueReceive(s_enc_q, &k, portMAX_DELAY);
        if (k < 0) goto chicos;            // nadie mira el 1080p: sólo la imagen chica y el juez
        int j = (s_jlatest + 1) & 1;
        if (j == s_jusb && !uvc_ready()) j = s_jlatest < 0 ? 0 : s_jlatest;   // el otro va viajando por USB
        jpeg_encode_cfg_t cfg = { .height = OH, .width = OW, .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                                  .sub_sample = s_422 ? JPEG_DOWN_SAMPLING_YUV422 : JPEG_DOWN_SAMPLING_YUV420,
                                  .image_quality = 85 };
        uint32_t len = 0;
        int64_t t0 = esp_timer_get_time();
        esp_err_t r = jpeg_encoder_process(s_jenc, &cfg, s_frame[k], OW * OH * 2, s_jpeg[j], s_jpeg_cap, &len);
        stats_busy(ENG_JENC, t0, esp_timer_get_time());
        xSemaphoreGive(s_free_frames);     // el cuadro 1080p ya se puede reescribir
        if (r != ESP_OK) {
            if (!s_422) { s_422 = true; ESP_LOGW(TAG, "JPEG 4:2:0 falló, uso 4:2:2"); }
            continue;
        }
        xSemaphoreTake(s_jmux, portMAX_DELAY);
        s_jpeg_len[j] = len; s_jlatest = j; s_jseq++;
        xSemaphoreGive(s_jmux);
        if (uvc_ready() && uvc_send(s_jpeg[j], len)) s_jusb = j;
        stats_frame();
    chicos:
        if (s_chico_listo) {
            jpeg_encode_cfg_t cc = { .height = CH_H, .width = TW, .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                                     .sub_sample = JPEG_DOWN_SAMPLING_YUV420, .image_quality = 65 };   // ~12 KB
            uint32_t clen = 0;
            int64_t tc0 = esp_timer_get_time();
            if (jpeg_encoder_process(s_jenc, &cc, s_chico, (uint32_t)(TW * CH_H * 2), s_jchico, s_jchico_cap, &clen) == ESP_OK) {
                xSemaphoreTake(s_jmux, portMAX_DELAY);
                s_jchico_len = clen;
                s_jchico_seq++;
                xSemaphoreGive(s_jmux);
            }
            stats_busy(ENG_JENC, tc0, esp_timer_get_time());
            s_chico_listo = false;
        }
        if (!s_gray_ready) continue;
        // el juez: JPEG en gris de 480x270, así la paleta del día y la noche no cambia la medición
        jpeg_encode_cfg_t gc = { .height = TH, .width = TW, .src_type = JPEG_ENCODE_IN_FORMAT_GRAY,
                                 .sub_sample = JPEG_DOWN_SAMPLING_GRAY, .image_quality = 85 };
        uint32_t glen = 0;
        t0 = esp_timer_get_time();
        r = jpeg_encoder_process(s_jenc, &gc, s_gray, TW * TH, s_gray_jpeg, s_gray_jpeg_cap, &glen);
        stats_busy(ENG_JENC, t0, esp_timer_get_time());
        s_gray_ready = false;
        if (r != ESP_OK) continue;
        float day = lpcore_day_phase() / 65536.0f;
        portENTER_CRITICAL(&s_gmux);
        uint32_t acc0 = s_evo.accepted;
        int gen = evo_feed(&s_evo, glen / 1024.0f, day, JUDGE_EVERY);
        evo_t e = s_evo;
        portEXIT_CRITICAL(&s_gmux);
        if (gen) tv_evento_evolucion(e.accepted > acc0);
        if (gen) {
            ESP_LOGI(TAG, "generación %lu: candidato %.1f KB vs actual %.1f KB, objetivo %.1f KB → %s (aceptadas %lu)",
                     (unsigned long)e.generation, e.last_kb_trial, e.last_kb_inc, e.last_target,
                     e.accepted > acc0 ? "aceptado" : "descartado", (unsigned long)e.accepted);
        }
    }
}

static volatile int64_t s_full_pedido_us;          // último que pidió el video 1080p por la red
size_t bioma_copy_jpeg(uint8_t *dst, size_t cap, uint32_t *seq_io, TickType_t wait)
{
    s_full_pedido_us = esp_timer_get_time();
    TickType_t t0 = xTaskGetTickCount();
    while (s_jseq == *seq_io || s_jlatest < 0) {
        if (xTaskGetTickCount() - t0 >= wait) return 0;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    xSemaphoreTake(s_jmux, portMAX_DELAY);
    size_t n = s_jpeg_len[s_jlatest];
    if (n <= cap) memcpy(dst, s_jpeg[s_jlatest], n); else n = 0;
    *seq_io = s_jseq;
    xSemaphoreGive(s_jmux);
    return n;
}

size_t bioma_copy_jpeg_chico(uint8_t *dst, size_t cap, uint32_t *seq_io, TickType_t wait)
{
    s_chico_pedido_us = esp_timer_get_time();
    TickType_t t0 = xTaskGetTickCount();
    while (!s_jchico || s_jchico_seq == *seq_io || !s_jchico_len) {
        if (xTaskGetTickCount() - t0 >= wait) return 0;
        s_chico_pedido_us = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreTake(s_jmux, portMAX_DELAY);
    size_t n = s_jchico_len;
    if (n <= cap) memcpy(dst, s_jchico, n); else n = 0;
    *seq_io = s_jchico_seq;
    xSemaphoreGive(s_jmux);
    return n;
}

void bioma_status(char *json, size_t cap)
{
    portENTER_CRITICAL(&s_gmux);
    evo_t e = s_evo;
    portEXIT_CRITICAL(&s_gmux);
    const genome_t *g = evo_current(&e);
    float day = lpcore_day_phase() / 65536.0f;
    static const char *modos[] = { "juez JPEG", "azar (control)", "congelado (control)" };
    snprintf(json, cap,
             "{\"agentes\":%d,\"generacion\":%lu,\"aceptadas\":%lu,\"modo\":\"%s\",\"midiendo\":\"%s\","
             "\"objetivo_kb\":%.1f,\"base_kb\":%.1f,\"kb_actual\":%.1f,\"kb_candidato\":%.1f,"
             "\"dia\":%.3f,\"velocidad_dia\":%lu,"
             "\"genoma\":{\"sa\":%.3f,\"ra\":%.3f,\"sd\":%.2f,\"ss\":%.2f,\"decay\":%.3f,\"dep\":%.2f,\"respawn\":%.4f},"
             "\"fps\":%.1f,\"cpu\":[%.0f,%.0f],\"jpeg_enc\":%.0f,\"ppa\":%.0f,\"usb\":%.0f,\"temp\":%.1f,\"uvc\":%s,"
             "\"reloj\":%s,\"hora_p4\":\"%02d:%02d\",\"ms\":{\"agentes\":%.1f,\"difusion\":%.1f,\"color\":%.1f,\"espera_ppa\":%.1f,\"espera_bufer\":%.1f,\"cuadro\":%.1f,\"orden\":%.1f}}",
             N_AGENTS, (unsigned long)e.generation, (unsigned long)e.accepted, modos[e.mode],
             e.phase ? "candidato" : "actual", e.base_kb > 0 ? evo_target(&e, day) : 0.0f, e.base_kb,
             e.last_kb_inc, e.last_kb_trial, day, (unsigned long)lpcore_speed(),
             g->sa, g->ra, g->sd, g->ss, g->decay, g->dep, g->respawn, stats_fps(), stats_cpu_pct(0), stats_cpu_pct(1),
             stats_eng_pct(ENG_JENC), stats_eng_pct(ENG_PPA), stats_eng_pct(ENG_USB), stats_temp(),
             uvc_streaming() ? "true" : "false",
             s_reloj_on ? "true" : "false", hora_segundos() >= 0 ? hora_segundos() / 3600 : 0,
             hora_segundos() >= 0 ? (hora_segundos() / 60) % 60 : 0,
             s_t_agents, s_t_diffuse, s_t_color, s_t_wait_ppa, s_t_wait_buf, s_t_frame, s_t_sort);
}

// ---------------- bucle principal (núcleo 0) ----------------
static inline float ms_since(int64_t t) { return (esp_timer_get_time() - t) / 1000.0f; }

// ---------------- el dedo en la pantalla táctil ----------------
// Donde está el dedo se deja un rastro muy fuerte: los agentes Physarum siguen el rastro, así que la colonia entera
// corre hacia el dedo y lo "persigue" si se arrastra. Se aplica entre pasos (los 2 núcleos están quietos).
static volatile int32_t s_toque_xy = -1;           // (x << 16) | y en celdas del rastro; -1 = nadie
static volatile int64_t s_toque_us;
void bioma_toque(int x, int y)
{
    if (x < 0 || y < 0 || x >= TW || y >= TH) return;
    s_toque_us = esp_timer_get_time();
    s_toque_xy = (x << 16) | y;
}

static void mancha(int cx, int cy)
{
    const int R = 9;
    for (int dy = -R; dy <= R; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= TH) continue;
        uint16_t *fila = sim_row[y];
        for (int dx = -R; dx <= R; dx++) {
            int x = cx + dx, d2 = dx * dx + dy * dy;
            if (x < 0 || x >= TW || d2 > R * R) continue;
            uint32_t v = fila[x] + (uint32_t)(26000 * (R * R - d2) / (R * R));
            fila[x] = (uint16_t)(v > 65535 ? 65535 : v);
        }
    }
}

// escena: 0 = BIOMA (la colonia), 1 = TIERRA VIVA. Se guarda en la NVS.
static volatile int s_escena = 1;
int bioma_escena(void) { return s_escena; }
void bioma_set_escena(int e)
{
    s_escena = e ? 1 : 0;
    nvs_handle_t h;
    if (nvs_open("lcd", NVS_READWRITE, &h) == ESP_OK) { nvs_set_i32(h, "escena", s_escena); nvs_commit(h); nvs_close(h); }
}

// en TIERRA VIVA el dedo gira el globo
static void toque_tierra(void)
{
    int32_t v = s_toque_xy;
    bool fresco = v >= 0 && esp_timer_get_time() - s_toque_us < 150000;
    tierra_dedo(fresco, fresco ? v >> 16 : 0, fresco ? v & 0xFFFF : 0);
}

static void aplicar_toque(void)
{
    static int lx = -1, ly = -1;
    int32_t v = s_toque_xy;
    if (v < 0 || esp_timer_get_time() - s_toque_us > 150000) { lx = -1; return; }
    int x = v >> 16, y = v & 0xFFFF;
    if (lx < 0) { lx = x; ly = y; }
    int dx = x - lx, dy = y - ly, n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 3 + 1;
    for (int i = 1; i <= n; i++) mancha(lx + dx * i / n, ly + dy * i / n);   // el trazo, sin huecos
    lx = x; ly = y;
}

static void bioma_task(void *arg)
{
    int64_t last_pal = 0, t_frame = esp_timer_get_time();
    bool ppa_busy = false;
    int ppa_k = 0;
    uint32_t frames = 0;
    while (1) {
        s_tierra = s_escena == 1 ? tierra_preparar_cuadro(TW, TH) : NULL;
        bool tierra = s_tierra != NULL;
        if (tierra) {                      // la colonia queda en pausa (sin gastar CPU) mientras se ve el globo
            toque_tierra();
            int64_t t0 = esp_timer_get_time();
            s_dibujo = !s_small2 ? s_small : s_small == s_small2 ? s_small_a : s_small2;
            baldosas_tierra();
            run_job(JOB_TIERRA);           // en la imagen libre: el PPA todavía escala la anterior
            if (s_tt[0]) esp_cache_msync(s_dibujo, s_small_cap, ESP_CACHE_MSYNC_FLAG_DIR_M2C);   // lo escribió el DMA
            tierra_marcas(s_tierra, (uint16_t *)s_dibujo, TW, TH);
            tierra_textos((uint16_t *)s_dibujo, TW, TH);
            s_t_agents = ms_since(t0); s_t_diffuse = 0;
        }
        s_cur = genome_now();
        float ta = 0, td = 0;
        for (int st = 0; st < (tierra ? 0 : STEPS_PER_FRAME); st++) {     // mientras tanto el PPA y el JPEG trabajan solos
            int64_t t0 = esp_timer_get_time();
            run_job(JOB_AGENTS);
            ta += ms_since(t0);
            t0 = esp_timer_get_time();
            run_job(JOB_DIFFUSE);
            td += ms_since(t0);
        }
        if (!tierra) { s_t_agents = ta; s_t_diffuse = td; aplicar_toque(); }
        if (SORT_EVERY && frames % SORT_EVERY == 0) {   // (probado: el paso de agentes es cálculo, no caché)
            int64_t t0 = esp_timer_get_time();
            sim_sort_agents(s_ag, s_ag_tmp, N_AGENTS);
            agent_t *t = s_ag; s_ag = s_ag_tmp; s_ag_tmp = t;
            s_t_sort = ms_since(t0);
        }
        int64_t now = esp_timer_get_time();
        if (now - last_pal > 1000000) {    // la paleta sigue al "sol" del núcleo LP (0 = medianoche)
            float ph = lpcore_day_phase() / 65536.0f;
            sim_build_palette(0.5f - 0.5f * cosf(2 * (float)M_PI * ph));
            tv_audio_dia(ph);
            last_pal = now;
        }
        int64_t tw = esp_timer_get_time();
        if (ppa_busy) {                    // el escalado anterior terminó → al codificador
            xSemaphoreTake(s_ppa_done, portMAX_DELAY);
            stats_busy(ENG_PPA, s_ppa_t0, s_ppa_t1);
            xQueueSend(s_enc_q, &ppa_k, portMAX_DELAY);
            ppa_busy = false;
        }
        s_t_wait_ppa = ms_since(tw);
        int64_t tc = esp_timer_get_time();
        tv_esperar_libre();                // la tele terminó de leer el cuadro anterior (normalmente ya)
        int64_t hms = tierra ? -1 : hora_ms();
        if (tierra) s_small = s_dibujo;    // el PPA ya soltó la anterior: la recién dibujada pasa a ser la imagen
        else run_job(JOB_COLOR);         // coloreado en los 2 núcleos (el PPA ya no lee s_small)
        if (s_reloj_on && hms >= 0) {      // el reloj va sobre la imagen chica: sale igual en la webcam y en la página
            int hs = (int)(hms / 1000);
            reloj_preparar(hs / 3600, (hs / 60) % 60, hs % 60);
            reloj_dibujar((uint16_t *)s_small, sim_palette(200), hs % 2 == 0, (int)(hms % 60000));
        }
        esp_cache_msync(s_small, s_small_cap, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        tv_cuadro_listo((const uint16_t *)s_small, TW, TH);
        lcd_cuadro_listo((const uint16_t *)s_small);           // la pantalla táctil de 2,8"   // la tele lo convierte a NTSC mientras los núcleos simulan
        if (s_chico && !s_chico_listo && esp_timer_get_time() - s_chico_pedido_us < 3000000) {
            memcpy(s_chico, s_small + (size_t)7 * TW * 2, (size_t)CH_H * TW * 2);   // filas 7..262
            esp_cache_msync(s_chico, s_chico_cap, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            s_chico_listo = true;
        }
        if (!tierra && ++frames % JUDGE_EVERY == 0 && !s_gray_ready) {
            sim_gray(s_gray, s_wk[0].hist, s_wk[1].hist);
            esp_cache_msync(s_gray, s_gray_cap, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            s_gray_ready = true;
        }
        s_t_color = ms_since(tc);
        int64_t tb = esp_timer_get_time();
        bool mirando = uvc_streaming() || tb - s_full_pedido_us < 5000000;   // webcam abierta o video 1080p en la página
        if (mirando) {
            xSemaphoreTake(s_free_frames, portMAX_DELAY);
            s_t_wait_buf = ms_since(tb);
            ppa_k = (ppa_k + 1) % N_FRAMES;
            ppa_busy = upscale_start(ppa_k);
            if (!ppa_busy) xSemaphoreGive(s_free_frames);
        } else {                           // nadie lo ve: ni PPA x4 ni JPEG 1080p (la memoria queda para dibujar)
            s_t_wait_buf = 0;
            if (s_chico_listo || s_gray_ready) { int nk = -1; xQueueSend(s_enc_q, &nk, 0); }
            stats_frame();
        }
        s_t_frame = ms_since(t_frame);
        t_frame = esp_timer_get_time();
        static int64_t t_log;
        if (t_frame - t_log > 10000000) {  // desglose del cuadro en la consola (sin red también se puede medir)
            t_log = t_frame;
            ESP_LOGI(TAG, "%s: cuadro %.1f ms = vida/globo %.1f + color/resto %.1f + espera PPA %.1f + espera búfer %.1f",
                     tierra ? "TIERRA" : "BIOMA", s_t_frame, s_t_agents + s_t_diffuse, s_t_color, s_t_wait_ppa, s_t_wait_buf);
            if (tierra) ESP_LOGI(TAG, "TIERRA: globo núcleo 0 %.1f ms, núcleo 1 %.1f ms", s_tt_ms[0], s_tt_ms[1]);
        }
    }
}

// Primero que todo (antes de USB y red): el rastro necesita 253 KB de RAM interna.
// Como la RAM interna viene en regiones separadas, se reserva en trozos de filas: se intenta el trozo más
// grande posible y, si no cabe, la mitad, y así (la tabla de filas hace que no importe dónde quede cada trozo).
void bioma_early_init(void)
{
    int y = 0, chunk = TH, pieces = 0;
    while (y < TH) {
        int n = chunk < TH - y ? chunk : TH - y;
        uint16_t *p = heap_caps_calloc(1, (size_t)n * TW * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) {
            if (chunk == 1) {
                // en este firmware el Wi-Fi y la tele también piden RAM interna: antes que caerse, lo que falta va a
                // PSRAM (esas filas se simulan más lento, pero BIOMA sigue vivo)
                ESP_LOGW(TAG, "no cabe el rastro en RAM interna: %d filas van a PSRAM", TH - y);
                uint16_t *q = heap_caps_calloc(1, (size_t)(TH - y) * TW * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
                if (!q) abort();
                for (int i = 0; i < TH - y; i++) sim_row[y + i] = q + i * TW;
                pieces++;
                break;
            }
            chunk = (chunk + 1) / 2;
            continue;
        }
        for (int i = 0; i < n; i++) sim_row[y + i] = p + i * TW;
        y += n;
        pieces++;
    }
    ESP_LOGI(TAG, "rastro %dx%d en %d trozo(s) de RAM interna", TW, TH, pieces);
}

void bioma_start(void)
{
    sim_init_tables();
    s_ag = heap_caps_malloc(N_AGENTS * sizeof(agent_t), MALLOC_CAP_SPIRAM);
    s_ag_tmp = heap_caps_malloc(N_AGENTS * sizeof(agent_t), MALLOC_CAP_SPIRAM);
    s_small_cap = P4_ALIGN(TW * TH * 2, 64);
    s_small = heap_caps_aligned_calloc(64, 1, s_small_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_small_a = s_small;
    s_small2 = heap_caps_aligned_calloc(64, 1, s_small_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_gray_cap = P4_ALIGN(TW * 272, 64);    // 2 filas de relleno: el JPEG trabaja en bloques de 8
    s_gray = heap_caps_aligned_calloc(64, 1, s_gray_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (s_gray) esp_cache_msync(s_gray, s_gray_cap, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_frame_cap = P4_ALIGN(OW * OH16 * 2, 64);
    for (int i = 0; i < N_FRAMES; i++) {             // el PPA escribe estos cuadros por DMA: que los ceros no queden en la caché
        s_frame[i] = heap_caps_aligned_calloc(64, 1, s_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (s_frame[i]) esp_cache_msync(s_frame[i], s_frame_cap, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    jpeg_encode_engine_cfg_t ecfg = { .timeout_ms = 300 };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&ecfg, &s_jenc));
    jpeg_encode_memory_alloc_cfg_t em = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    for (int i = 0; i < 2; i++) s_jpeg[i] = jpeg_alloc_encoder_mem(768 * 1024, &em, &s_jpeg_cap);
    s_gray_jpeg = jpeg_alloc_encoder_mem(128 * 1024, &em, &s_gray_jpeg_cap);
    s_jchico = jpeg_alloc_encoder_mem(128 * 1024, &em, &s_jchico_cap);
    s_chico_cap = P4_ALIGN(TW * CH_H * 2, 64);
    s_chico = heap_caps_aligned_calloc(64, 1, s_chico_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ESP_ERROR_CHECK(ppa_register_client(&pc, &s_srm));
    ppa_event_callbacks_t cbs = { .on_trans_done = on_ppa_done };
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_srm, &cbs));
    s_jmux = xSemaphoreCreateMutex();
    s_ppa_done = xSemaphoreCreateBinary();
    s_free_frames = xSemaphoreCreateCounting(N_FRAMES, N_FRAMES);
    s_enc_q = xQueueCreate(N_FRAMES, sizeof(int));
    if (!s_ag || !s_ag_tmp || !s_small || !s_gray || !s_gray_jpeg || !s_frame[N_FRAMES - 1] || !s_jpeg[1] || !s_jmux || !s_ppa_done || !s_free_frames || !s_enc_q) {
        ESP_LOGE(TAG, "falta memoria (PSRAM libre %u KB)", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        abort();
    }
    // semilla del generador por hardware y después un xorshift: con esp_random() para cada uno de los 600.000
    // números, el arranque tomaba 40 s medidos (cada llamada espera entropía)
    nvs_handle_t nh;
    if (nvs_open("lcd", NVS_READONLY, &nh) == ESP_OK) { int32_t e; if (nvs_get_i32(nh, "escena", &e) == ESP_OK) s_escena = e ? 1 : 0; nvs_close(nh); }
    tierra_start();
    s_seed_state = esp_random() | 1;
    sim_seed_agents(s_ag, N_AGENTS, fast_rnd);
    evo_init(&s_evo, &G0, 450, esp_random(), EVO_JPEG);   // ventanas de 450 cuadros (~30 s a 15 fps)
    for (int k = 0; k < 2; k++) {
        s_wk[k].core = k;
        s_wk[k].a_first = k * (N_AGENTS / 2);
        s_wk[k].a_last = (k + 1) * (N_AGENTS / 2);
        s_wk[k].go = xSemaphoreCreateBinary();
        s_wk[k].done = xSemaphoreCreateBinary();
        xTaskCreatePinnedToCore(worker_task, k ? "vida1" : "vida0", 8192, &s_wk[k], 8, NULL, k);
    }
    xTaskCreatePinnedToCore(enc_task, "jpeg", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(bioma_task, "bioma", 6144, NULL, 9, NULL, 0);
    ESP_LOGI(TAG, "BIOMA vivo: %d agentes, rastro %dx%d en RAM interna, salida %dx%d", N_AGENTS, TW, TH, OW, OH);
}
