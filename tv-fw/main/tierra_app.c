// tierra_app.c — TIERRA VIVA en el P4: texturas en PSRAM, datos vivos por internet, el dedo y los textos.
//   - sismos M2.5+ de las últimas 24 h (USGS), cada 10 min
//   - la Estación Espacial Internacional (wheretheiss.at), cada 5 s, con su estela de la última media hora
//   - las nubes reales del planeta (clouds.matteason.co.uk, se renuevan cada 3 h), decodificadas con el JPEG por hardware
//   - la hora exacta sale de las mismas respuestas (cabecera Date) y el sol se pone donde está de verdad
// El dibujo en sí está en tierra.c (C puro, probado en el PC con tierra/host_test).
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "driver/jpeg_decode.h"
#include "tierra.h"
#include "tierra_app.h"
#include "red.h"
#include "letra.h"

static const char *TAG = "tierra";

extern const uint8_t dia_start[] asm("_binary_dia565_bin_start");
extern const uint8_t luces_start[] asm("_binary_luces8_bin_start");
extern const uint8_t mar_start[] asm("_binary_mar8_bin_start");
extern const uint8_t nubes_start[] asm("_binary_nubes8_bin_start");

#define NPX (TIERRA_TW * TIERRA_TH)
static uint32_t *s_tex;                 // las 4 texturas juntas (ver tierra.h), 2 MB en PSRAM
static tierra_px_t *s_tabla;            // la tabla por píxel del disco (ver tierra.h), 620 KB en PSRAM
static float s_tabla_pitch = 99, s_tabla_r;
static bool s_listo;

static SemaphoreHandle_t s_mux;
EXT_RAM_BSS_ATTR static tierra_t s_vivo;                 // datos vivos (protegidos por s_mux)
EXT_RAM_BSS_ATTR static tierra_t s_cuadro;               // la copia de este cuadro (la leen los 2 núcleos)
static char s_linea_sismos[96], s_linea_eei[96], s_linea_sel[112], s_linea_extra[112];
static int64_t s_sel_hasta;
EXT_RAM_BSS_ATTR static char s_lugar[TIERRA_MAX_SISMOS][44];
static volatile int s_nubes_ok, s_sismos_ok, s_eei_n;
// aviones: dos juegos (se lee uno mientras se llena el otro)
EXT_RAM_BSS_ATTR static tierra_avion_t s_av[2][TIERRA_MAX_AVIONES];
static volatile int s_av_i, s_av_n;
static volatile double s_av_t;              // hora unix de las posiciones
static volatile int64_t s_nubes_us, s_sismos_us;
// EEI: dos últimas posiciones para moverla suave entre consultas
static double s_eei_t0, s_eei_t1;
static float s_eei_la0, s_eei_lo0, s_eei_la1, s_eei_lo1;

// vista (sólo la toca bioma_task)
static float s_yaw = -71.0f * (float)M_PI / 180, s_pitch = -28.0f * (float)M_PI / 180, s_vyaw, s_vpitch;
static int64_t s_ultimo_toque, s_t_ant;
static int s_tx0, s_ty0, s_lx = -1, s_ly = -1;
static int64_t s_toque_desde;
// zoom: la barra del borde derecho de la pantalla (arriba = cerca). Se acerca suave hacia el objetivo.
static float s_zoom = 1.0f, s_zoom_obj = 1.0f;
static bool s_en_barra;
#define ZOOM_MAX 6.0f
#define BARRA_X (TIERRA_LCD_X + 320 - 30)
#define BARRA_Y0 (TIERRA_LCD_Y + 52)
#define BARRA_Y1 (TIERRA_LCD_Y + 240 - 44)

// ---------------- datos vivos ----------------
static const char *num(const char *p, const char *clave, double *v)
{
    p = strstr(p, clave);
    if (!p) return NULL;
    p += strlen(clave);
    char *e;
    *v = strtod(p, &e);
    return e;
}

static void traer_eei(char *buf, int cap)
{
    int c;
    if (red_https_get("api.wheretheiss.at", "/v1/satellites/25544", buf, cap, &c) < 0) return;
    double la, lo, alt, ts, vel;
    const char *b = buf + c;
    if (!num(b, "\"latitude\":", &la) || !num(b, "\"longitude\":", &lo) || !num(b, "\"altitude\":", &alt) || !num(b, "\"timestamp\":", &ts)) return;
    if (!num(b, "\"velocity\":", &vel)) vel = 27600;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_eei_t0 = s_eei_t1; s_eei_la0 = s_eei_la1; s_eei_lo0 = s_eei_lo1;
    s_eei_t1 = ts; s_eei_la1 = (float)la; s_eei_lo1 = (float)lo;
    s_vivo.eei_alt_km = (float)alt;
    if (s_eei_n == 0) { s_eei_t0 = ts - 5; s_eei_la0 = (float)la; s_eei_lo0 = (float)lo; }
    // la estela: un punto cada 20 s (media hora = 90 puntos)
    int n = s_vivo.n_estela;
    if (n == 0 || s_eei_n % 4 == 0) {
        if (n == TIERRA_MAX_ESTELA) { memmove(s_vivo.estela[0], s_vivo.estela[1], sizeof(s_vivo.estela[0]) * (n - 1)); n--; }
        s_vivo.estela[n][0] = (float)la; s_vivo.estela[n][1] = (float)lo;
        s_vivo.n_estela = n + 1;
    }
    s_vivo.eei_ok = 1;
    snprintf(s_linea_eei, sizeof(s_linea_eei), "EEI %.0f km/h a %.0f km · %.1f°%c %.1f°%c", vel, alt,
             fabs(la), la >= 0 ? 'N' : 'S', fabs(lo), lo >= 0 ? 'E' : 'O');
    s_eei_n++;
    xSemaphoreGive(s_mux);
}

// estela inicial: las posiciones de la última media hora de una sola vez
static void traer_estela(char *buf, int cap)
{
    time_t ahora = time(NULL);
    if (ahora < 1700000000) return;
    char path[240];
    int k = snprintf(path, sizeof(path), "/v1/satellites/25544/positions?timestamps=");
    for (int i = 0; i < 10; i++) k += snprintf(path + k, sizeof(path) - k, "%s%lld", i ? "," : "", (long long)(ahora - 1800 + i * 180));
    int c;
    if (red_https_get("api.wheretheiss.at", path, buf, cap, &c) < 0) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const char *p = buf + c;
    int n = 0;
    double la, lo;
    while (n < 10 && (p = num(p, "\"latitude\":", &la)) && (p = num(p, "\"longitude\":", &lo))) {
        // entre dos puntos cada 3 min se intercalan 8 (así la estela sale curva y no a trozos)
        if (n > 0) {
            float la0 = s_vivo.estela[s_vivo.n_estela - 1][0], lo0 = s_vivo.estela[s_vivo.n_estela - 1][1];
            float dlo = (float)lo - lo0;
            if (dlo > 180) dlo -= 360; else if (dlo < -180) dlo += 360;
            for (int j = 1; j < 9 && s_vivo.n_estela < TIERRA_MAX_ESTELA; j++) {
                float f = j / 9.0f, l = lo0 + dlo * f;
                if (l > 180) l -= 360; else if (l < -180) l += 360;
                s_vivo.estela[s_vivo.n_estela][0] = la0 + ((float)la - la0) * f;
                s_vivo.estela[s_vivo.n_estela][1] = l;
                s_vivo.n_estela++;
            }
        }
        if (s_vivo.n_estela < TIERRA_MAX_ESTELA) {
            s_vivo.estela[s_vivo.n_estela][0] = (float)la; s_vivo.estela[s_vivo.n_estela][1] = (float)lo;
            s_vivo.n_estela++;
        }
        n++;
    }
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "estela de la EEI: %d puntos", s_vivo.n_estela);
}

static void traer_sismos(char *buf, int cap)
{
    int c;
    if (red_https_get("earthquake.usgs.gov", "/earthquakes/feed/v1.0/summary/2.5_day.geojson", buf, cap, &c) < 0) return;
    EXT_RAM_BSS_ATTR static tierra_sismo_t q[TIERRA_MAX_SISMOS];
    EXT_RAM_BSS_ATTR static char lugar[TIERRA_MAX_SISMOS][44];
    int n = 0, imax = -1;
    double ahora_ms = (double)time(NULL) * 1000.0;
    const char *p = buf + c;
    while (n < TIERRA_MAX_SISMOS) {
        const char *f = strstr(p, "\"type\":\"Feature\"");
        if (!f) break;
        const char *sig = strstr(f + 10, "\"type\":\"Feature\"");
        double mag, t = ahora_ms, lo, la;
        const char *m = num(f, "\"mag\":", &mag);
        const char *cc = strstr(f, "\"coordinates\":[");
        if (!m || !cc || (sig && cc > sig)) { p = f + 10; continue; }
        num(f, "\"time\":", &t);
        char *e;
        lo = strtod(cc + 15, &e);
        la = strtod(e + 1, NULL);
        q[n].lat = (float)la; q[n].lon = (float)lo; q[n].mag = (float)mag;
        q[n].edad_h = (float)((ahora_ms - t) / 3600000.0);
        if (q[n].edad_h < 0) q[n].edad_h = 0;
        lugar[n][0] = 0;
        const char *pl = strstr(f, "\"place\":\"");
        if (pl && (!sig || pl < sig)) {
            pl += 9;
            int k = 0;
            while (*pl && *pl != '"' && k < 43) lugar[n][k++] = *pl++;
            lugar[n][k] = 0;
        }
        if (imax < 0 || mag > q[imax].mag) imax = n;
        n++;
        p = cc;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(s_vivo.sismos, q, sizeof(q[0]) * n);
    memcpy(s_lugar, lugar, sizeof(lugar[0]) * n);
    s_vivo.n_sismos = n;
    if (imax >= 0) snprintf(s_linea_sismos, sizeof(s_linea_sismos), "%d sismos en 24 h · mayor M%.1f %.40s", n, q[imax].mag, lugar[imax]);
    else snprintf(s_linea_sismos, sizeof(s_linea_sismos), "sin sismos M2.5+ en 24 h");
    xSemaphoreGive(s_mux);
    s_sismos_ok = 1;
    s_sismos_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%d sismos (USGS)", n);
}

static void traer_nubes(char *buf, int cap)
{
    static jpeg_decoder_handle_t dec;
    static uint8_t *salida;
    static size_t salida_cap;
    int c, n = red_https_get("clouds.matteason.co.uk", "/images/1024x512/clouds.jpg", buf, cap, &c);
    if (n < 0) return;
    int largo = n - c;
    memmove(buf, buf + c, largo);
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info((const uint8_t *)buf, largo, &info) != ESP_OK || info.width != TIERRA_TW || info.height != TIERRA_TH) {
        ESP_LOGW(TAG, "nubes: la imagen no es 1024x512");
        return;
    }
    if (!dec) {
        jpeg_decode_engine_cfg_t ec = { .timeout_ms = 500 };
        if (jpeg_new_decoder_engine(&ec, &dec) != ESP_OK) return;
        jpeg_decode_memory_alloc_cfg_t mc = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        salida = jpeg_alloc_decoder_mem(NPX * 2, &mc, &salida_cap);
        if (!salida) return;
    }
    jpeg_decode_cfg_t dc = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR };
    uint32_t out = 0;
    esp_cache_msync(buf, (largo + 63) & ~63, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    int64_t t0 = esp_timer_get_time();
    if (jpeg_decoder_process(dec, &dc, (const uint8_t *)buf, largo, salida, salida_cap, &out) != ESP_OK) { ESP_LOGW(TAG, "nubes: el JPEG no se pudo decodificar"); return; }
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    const uint16_t *px = (const uint16_t *)salida;
    uint8_t *t8 = (uint8_t *)s_tex;
    for (int i = 0; i < NPX; i++) {       // se reemplaza sólo el byte de las nubes de cada téxel
        uint16_t v = px[i];
        t8[i * 4 + 2] = (uint8_t)((((v >> 11) << 3) * 5 + (((v >> 5) & 63) << 2) * 9 + ((v & 31) << 3) * 2) >> 4);
    }
    s_nubes_ok = 1;
    s_nubes_us = esp_timer_get_time();
    ESP_LOGI(TAG, "nubes reales del planeta: %d KB de JPEG, decodificadas por hardware en %d ms", largo / 1024, ms);
}

static void traer_aviones(char *buf, int cap)
{
    int c;
    int64_t t0 = esp_timer_get_time();
    int n = red_https_get("opensky-network.org", "/api/states/all", buf, cap, &c);
    if (n < 0) return;
    int bajada_ms = (int)((esp_timer_get_time() - t0) / 1000);
    double td;
    int k = s_av_i ^ 1;
    int64_t t1 = esp_timer_get_time();
    int na = tierra_leer_aviones(buf + c, s_av[k], TIERRA_MAX_AVIONES, &td);
    if (na <= 0) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_av_i = k; s_av_n = na; s_av_t = td;
    snprintf(s_linea_extra, sizeof(s_linea_extra), "%d aviones en vuelo ahora", na);
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "%d aviones en vuelo (OpenSky): %d KB bajados en %d ms, leídos en %d ms", na, (n - c) / 1024, bajada_ms,
             (int)((esp_timer_get_time() - t1) / 1000));
}

static void datos_task(void *arg)
{
    const int cap = 512 * 1024;
    jpeg_decode_memory_alloc_cfg_t mc = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t got = 0;
    char *buf = jpeg_alloc_decoder_mem(cap, &mc, &got);     // sirve también de entrada del decodificador
    if (!buf) { ESP_LOGE(TAG, "sin memoria para los datos"); vTaskDelete(NULL); }
    const int cap_av = 1400 * 1024;                          // la respuesta de OpenSky pesa ~1 MB
    char *buf_av = heap_caps_malloc(cap_av, MALLOC_CAP_SPIRAM);
    int64_t prox_eei = 0, prox_sismos = 0, prox_nubes = 0, prox_aviones = 0;
    bool estela = false;
    while (1) {
        int64_t t = esp_timer_get_time();
        if (t >= prox_eei) {
            traer_eei(buf, cap);
            prox_eei = t + (s_vivo.eei_ok ? 5000000LL : 15000000LL);
            if (s_vivo.eei_ok && !estela) { traer_estela(buf, cap); estela = true; }
        }
        if (t >= prox_sismos) { traer_sismos(buf, cap); prox_sismos = t + (s_sismos_ok ? 600000000LL : 20000000LL); }
        if (buf_av && t >= prox_aviones) {                   // sin cuenta, OpenSky deja ~100 consultas al día
            traer_aviones(buf_av, cap_av);
            prox_aviones = t + (s_av_n ? 15 * 60000000LL : 60000000LL);
        }
        if (t >= prox_nubes) { traer_nubes(buf, cap); prox_nubes = t + (s_nubes_ok ? 3 * 3600000000LL : 30000000LL); }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------- el dedo ----------------
// Arrastrar = girar el globo (con inercia). Tocar sin arrastrar = el sismo más cercano muestra su magnitud y lugar.
void tierra_dedo(bool apoyado, int x, int y)
{
    int64_t ahora = esp_timer_get_time();
    if (apoyado && (s_lx < 0 ? x >= BARRA_X && y >= BARRA_Y0 - 10 && y <= BARRA_Y1 + 10 : s_en_barra)) {
        s_en_barra = true;                                   // en la barra: sólo zoom (escala logarítmica)
        float f = 1.0f - (float)(y - BARRA_Y0) / (BARRA_Y1 - BARRA_Y0);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        s_zoom_obj = expf(f * logf(ZOOM_MAX));
        s_lx = x; s_ly = y; s_tx0 = -100; s_ultimo_toque = ahora;
        return;
    }
    if (apoyado) {
        if (s_lx < 0) { s_tx0 = x; s_ty0 = y; s_toque_desde = ahora; s_vyaw = s_vpitch = 0; }
        else {
            float dx = (float)(x - s_lx), dy = (float)(y - s_ly), R = s_cuadro.R > 0 ? s_cuadro.R : 110;
            s_yaw -= dx / R;
            s_pitch += dy / R;
            float dt = (ahora - s_ultimo_toque) / 1e6f;
            if (dt > 0.005f && dt < 0.3f) { s_vyaw = s_vyaw * 0.5f - 0.5f * dx / R / dt; s_vpitch = s_vpitch * 0.5f + 0.5f * dy / R / dt; }
        }
        s_lx = x; s_ly = y;
        s_ultimo_toque = ahora;
        return;
    }
    if (s_lx >= 0) {                                         // se soltó: ¿fue un toque corto sin arrastrar?
        int mx = abs(s_lx - s_tx0) + abs(s_ly - s_ty0);
        if (mx < 8 && ahora - s_toque_desde < 600000) {
            int mejor = -1;
            float md = 18.0f * 18.0f;
            for (int i = 0; i < s_cuadro.n_sismos; i++) {
                float sx, sy;
                if (!tierra_proyectar(&s_cuadro, s_cuadro.sismos[i].lat, s_cuadro.sismos[i].lon, 0, &sx, &sy)) continue;
                float d = (sx - s_tx0) * (sx - s_tx0) + (sy - s_ty0) * (sy - s_ty0);
                if (d < md) { md = d; mejor = i; }
            }
            xSemaphoreTake(s_mux, portMAX_DELAY);
            if (mejor >= 0) {
                const tierra_sismo_t *q = &s_cuadro.sismos[mejor];
                snprintf(s_linea_sel, sizeof(s_linea_sel), "M%.1f %.43s · hace %.0f h", q->mag, s_lugar[mejor], q->edad_h);
                s_sel_hasta = ahora + 8000000;
            } else {
                float la, lo;
                if (tierra_desproyectar(&s_cuadro, (float)s_tx0, (float)s_ty0, &la, &lo)) {
                    snprintf(s_linea_sel, sizeof(s_linea_sel), "tocaste %.1f°%c %.1f°%c", fabsf(la), la >= 0 ? 'N' : 'S', fabsf(lo), lo >= 0 ? 'E' : 'O');
                    s_sel_hasta = ahora + 4000000;
                }
            }
            xSemaphoreGive(s_mux);
        }
        s_lx = -1;
        s_en_barra = false;
    }
}

// ---------------- cada cuadro ----------------
const tierra_t *tierra_preparar_cuadro(int w, int h)
{
    if (!s_listo) return NULL;
    int64_t ahora = esp_timer_get_time();
    float dt = s_t_ant ? (ahora - s_t_ant) / 1e6f : 0;
    if (dt > 0.2f) dt = 0.2f;
    s_t_ant = ahora;
    if (s_lx < 0) {                                          // sin dedo: inercia y, después, giro lento solo
        s_yaw += s_vyaw * dt; s_pitch += s_vpitch * dt;
        float fr = expf(-2.2f * dt);
        s_vyaw *= fr; s_vpitch *= fr;
        if (fabsf(s_vpitch) < 0.02f) s_vpitch = 0;          // quieto de verdad: así la tabla deja de rehacerse
        if (ahora - s_ultimo_toque > 7000000) {
            s_yaw += 0.12f * dt;                             // ~7°/s
        }
    }
    if (s_pitch > 1.35f) s_pitch = 1.35f;
    if (s_pitch < -1.35f) s_pitch = -1.35f;
    if (s_yaw > (float)M_PI) s_yaw -= 2 * (float)M_PI;
    if (s_yaw < -(float)M_PI) s_yaw += 2 * (float)M_PI;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(&s_cuadro, &s_vivo, sizeof(s_cuadro));
    // la EEI entre consultas: se extrapola con la velocidad de las dos últimas posiciones
    if (s_vivo.eei_ok && s_eei_t1 > s_eei_t0) {
        double now = (double)time(NULL) + (double)(ahora % 1000000) / 1e6;
        float f = (float)((now - s_eei_t1) / (s_eei_t1 - s_eei_t0));
        if (f > 4) f = 4;
        if (f < 0) f = 0;
        float dlo = s_eei_lo1 - s_eei_lo0;
        if (dlo > 180) dlo -= 360; else if (dlo < -180) dlo += 360;
        s_cuadro.eei_lat = s_eei_la1 + (s_eei_la1 - s_eei_la0) * f;
        float lo = s_eei_lo1 + dlo * f;
        if (lo > 180) lo -= 360; else if (lo < -180) lo += 360;
        s_cuadro.eei_lon = lo;
    } else if (s_vivo.eei_ok) { s_cuadro.eei_lat = s_eei_la1; s_cuadro.eei_lon = s_eei_lo1; }
    xSemaphoreGive(s_mux);
    time_t tt = time(NULL);
    s_zoom += (s_zoom_obj - s_zoom) * (1.0f - expf(-7.0f * dt));      // zoom suave
    if (fabsf(s_zoom - s_zoom_obj) < 0.002f) s_zoom = s_zoom_obj;
    float R = 106.0f * s_zoom;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cuadro.aviones = s_av[s_av_i]; s_cuadro.n_aviones = s_av_n;
    s_cuadro.aviones_dt = s_av_t > 0 && tt > 1700000000 ? (float)((double)tt + (ahora % 1000000) / 1e6 - s_av_t) : 0;
    if (s_cuadro.aviones_dt > 3600) s_cuadro.n_aviones = 0;             // datos de hace más de 1 h: no se muestran
    xSemaphoreGive(s_mux);
    s_cuadro.tex = s_tex;
    s_cuadro.tabla = s_zoom == 1.0f ? s_tabla : NULL;               // la tabla vale para el tamaño normal
    s_cuadro.tabla_rehacer = s_pitch != s_tabla_pitch || s_tabla_r != R;   // sólo si se inclinó el globo
    if (s_cuadro.tabla) { s_tabla_pitch = s_pitch; s_tabla_r = R; } else s_tabla_r = 0;
    s_cuadro.filtrar = s_zoom > 1.4f;
    // con zoom se dibuja sólo lo que ve la pantalla (y un poco más); en tamaño normal, la imagen entera
    bool recorte = s_zoom > 1.02f;
    s_cuadro.clip_x0 = recorte ? TIERRA_LCD_X : 0; s_cuadro.clip_x1 = recorte ? TIERRA_LCD_X + 320 : w;
    s_cuadro.clip_y0 = recorte ? TIERRA_LCD_Y : 0; s_cuadro.clip_y1 = recorte ? TIERRA_LCD_Y + 240 : h;
    s_cuadro.yaw = s_yaw; s_cuadro.pitch = s_pitch;
    s_cuadro.cx = TIERRA_LCD_X + 160; s_cuadro.cy = TIERRA_LCD_Y + 120; s_cuadro.R = R;   // zoom 1: entero en la pantalla, con su halo
    s_cuadro.t = ahora / 1e6f;
    double utc = tt > 1700000000 ? (double)tt + (ahora % 1000000) / 1e6 : 1790000000.0 + ahora / 1e6;   // sin hora: una cualquiera
    tierra_sol(utc, s_cuadro.sol);
    tierra_preparar(&s_cuadro);
    return &s_cuadro;
}

// ---------------- textos ----------------
// letra proporcional al tamaño real de la pantalla (letra.h, Segoe UI 13 px), con sombra
static int letra_i(uint32_t cp)
{
    for (int i = 0; i < LETRA_N; i++) if (letra_cp[i] == cp) return i;
    return '?' - 32;
}

static uint32_t sig_cp(const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint32_t cp = *p++;
    if (cp >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (cp >= 0xC0 && (p[0] & 0xC0) == 0x80) { cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F); p++; }
    *pp = p;
    return cp;
}

static int texto_ancho(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    int w = 0;
    while (*p) w += letra_w[letra_i(sig_cp(&p))] - 1;
    return w;
}

// dibuja hasta x_max (lo que no cabe se corta en una letra entera)
static void texto(uint16_t *img, int w, int h, int x, int y, const char *s, uint16_t color, int x_max)
{
    const uint8_t *p = (const uint8_t *)s;
    int cr = color >> 11, cg = (color >> 5) & 63, cb = color & 31;
    while (*p) {
        int gi = letra_i(sig_cp(&p)), gw = letra_w[gi];
        if (x + gw > x_max) break;
        const uint8_t *a = letra_a8 + letra_off[gi];
        for (int sh = 1; sh >= 0; sh--)                       // primero la sombra, después la letra
            for (int gy = 0; gy < LETRA_H; gy++) for (int gx = 0; gx < gw; gx++) {
                int al = a[gy * gw + gx];
                if (!al) continue;
                int X = x + gx + sh, Y = y + gy + sh;
                if (X < 0 || X >= w || Y < 0 || Y >= h) continue;
                uint16_t *o = &img[Y * w + X];
                int R = *o >> 11, G = (*o >> 5) & 63, B = *o & 31;
                int tr = sh ? 0 : cr, tg = sh ? 0 : cg, tb = sh ? 0 : cb, aa = sh ? al * 3 / 4 : al;
                R += (tr - R) * aa / 255; G += (tg - G) * aa / 255; B += (tb - B) * aa / 255;
                *o = (uint16_t)((R << 11) | (G << 5) | B);
            }
        x += gw - 1;
    }
}

// la pantalla táctil muestra 1:1 el recorte 320x240 desde (80,15) de la imagen chica: los textos van dentro
#define MX0 (TIERRA_LCD_X + 5)
#define MX1 (TIERRA_LCD_X + 320 - 5)
#define MY0 (TIERRA_LCD_Y + 3)
#define MY1 (TIERRA_LCD_Y + 240 - LETRA_H - 2)
void tierra_textos(uint16_t *img, int w, int h)
{
    char hora[48] = "sin hora";
    time_t tt = time(NULL);
    if (tt > 1700000000) {
        struct tm lt, ut;
        localtime_r(&tt, &lt);
        gmtime_r(&tt, &ut);
        snprintf(hora, sizeof(hora), "%02d:%02d Chile · %02d:%02d UTC", lt.tm_hour, lt.tm_min, ut.tm_hour, ut.tm_min);
    }
    texto(img, w, h, MX0, MY0, "TIERRA VIVA", 0xFFFF, MX1);
    texto(img, w, h, MX1 - texto_ancho(hora), MY0, hora, 0xBDF7, MX1);
    char a[112], b[112], c[112];
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool sel = esp_timer_get_time() < s_sel_hasta;
    strlcpy(a, sel ? s_linea_sel : s_linea_sismos, sizeof(a));
    strlcpy(b, s_linea_eei, sizeof(b));
    strlcpy(c, s_linea_extra, sizeof(c));
    xSemaphoreGive(s_mux);
    if (red_via() == 0 && !s_sismos_ok) {
        strlcpy(a, "sin internet: cable al HUSB + puente_internet", sizeof(a));
        b[0] = 0;
    }
    if (c[0]) texto(img, w, h, MX0, MY0 + LETRA_H, c, 0x8C71, MX1);
    // la barra del zoom (borde derecho): riel tenue y una perilla
    for (int y = BARRA_Y0; y <= BARRA_Y1; y++) for (int x = BARRA_X + 12; x < BARRA_X + 15; x++) {
        uint16_t *o = &img[y * w + x];
        *o = (uint16_t)(((*o >> 1) & 0x7BEF) + 0x2104);
    }
    int py = BARRA_Y1 - (int)(logf(s_zoom) / logf(ZOOM_MAX) * (BARRA_Y1 - BARRA_Y0));
    for (int dy = -5; dy <= 5; dy++) for (int dx = -5; dx <= 5; dx++) {
        int d2 = dx * dx + dy * dy;
        if (d2 > 25) continue;
        int X = BARRA_X + 13 + dx, Y = py + dy;
        if (X >= 0 && X < w && Y >= 0 && Y < h) img[Y * w + X] = d2 > 12 ? 0x7D7F : 0xFFFF;
    }
    if (s_zoom > 1.05f) {
        char z[16];
        snprintf(z, sizeof(z), "x%.1f", s_zoom);
        texto(img, w, h, BARRA_X - 8, BARRA_Y0 - LETRA_H - 4, z, 0xFFFF, MX1 + 5);
    }
    texto(img, w, h, MX0, MY1 - LETRA_H, a, sel ? 0xFFE0 : 0xFD20, MX1);
    texto(img, w, h, MX0, MY1, b, 0x9EFF, MX1);
}

void tierra_estado_json(char *buf, size_t cap)
{
    char r[200];
    red_estado_json(r, sizeof(r));
    snprintf(buf, cap, "{\"red\":%s,\"sismos\":%d,\"eei_consultas\":%d,\"estela\":%d,\"nubes_reales\":%s,\"yaw\":%.1f,\"pitch\":%.1f}",
             r, s_vivo.n_sismos, s_eei_n, s_vivo.n_estela, s_nubes_ok ? "true" : "false",
             s_yaw * 180 / M_PI, s_pitch * 180 / M_PI);
}

void tierra_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_tex = heap_caps_malloc(NPX * 4, MALLOC_CAP_SPIRAM);
    s_tabla = heap_caps_malloc(TIERRA_TABLA_BYTES, MALLOC_CAP_SPIRAM);   // si no hubiera, se calcula todo cada cuadro
    if (!s_mux || !s_tex) { ESP_LOGE(TAG, "sin PSRAM para las texturas"); return; }
    int64_t t0 = esp_timer_get_time();
    const uint16_t *dia = (const uint16_t *)dia_start;       // de la flash a la PSRAM, juntas: leerlas desde la flash sería lentísimo
    for (int i = 0; i < NPX; i++) s_tex[i] = tierra_texel(dia[i], nubes_start[i], luces_start[i], mar_start[i]);
    ESP_LOGI(TAG, "texturas de la NASA juntas en PSRAM (2 MB) en %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    strlcpy(s_linea_eei, "buscando la Estación Espacial...", sizeof(s_linea_eei));
    strlcpy(s_linea_sismos, "buscando sismos...", sizeof(s_linea_sismos));
    s_listo = true;
    red_start();
    xTaskCreatePinnedToCoreWithCaps(datos_task, "tierra_datos", 12288, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
}
