// tv.c — BIOMA en la tele antigua por el cable amarillo, sin PC.
//
// El motor: el periférico de pantalla del P4 (LCD_CAM) en modo i80 con "salida siempre activa" y el DMA en anillo
// repite sin parar un cuadro NTSC completo (262 × 910 muestras = 233 KB en PSRAM, ntsc_core.c). No hay borrado
// horizontal ni vertical por hardware: cada muestra del cuadro sale tal cual, así que los sincronismos, el burst
// y la imagen son exactamente los de la norma. El reloj sale del APLL: 114,545454 MHz / 2 / 4 = 14,318181 MHz,
// 4 veces la subportadora de color (sin divisor fraccionario, sin temblor).
//
// Pines: IO16..IO21 = los 6 bits del "DAC" de resistencias (bit 0 = IO16) → RCA amarillo. IO22 = sonido (tv_audio.c).
// IO28..IO31 quedan libres a propósito: ahí va montado un ESP32-C3 que le da Wi-Fi al P4 (ESP-Hosted por UART).
//
// Autoprueba (/api/tv/prueba): el mismo P4 se mira a sí mismo. El reloj de píxel sale por IO33 (sin nada conectado)
// y vuelve a entrar al contador de pulsos (frecuencia real) y al PARLIO RX, que graba lo que salen por IO16..IO23
// muestra a muestra. Esa grabación se compara byte a byte con el cuadro en memoria.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_clk_tree.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_private/esp_dma_utils.h"
#include "esp_private/gpio.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/lcd_ll.h"
#include "hal/lcd_periph.h"
#include "soc/gpio_sig_map.h"
#include "driver/parlio_rx.h"
#include "driver/pulse_cnt.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ntsc_core.h"
#include "tv.h"
#include "app.h"

static const char *TAG = "tv";

#define APLL_HZ       114545454          // 8 × 14.318.181,8 Hz
#define CAP_LEN       65535              // el PARLIO RX graba como máximo 65.535 muestras seguidas (72 líneas)
#define CAP_VECES     6

typedef struct {
    uint32_t   version;
    ntsc_cfg_t ntsc;
    int32_t    modo;        // 0 = BIOMA · 1 = carta de ajuste · 2 = negro · 3 = blanco
    int32_t    x0;          // columna de BIOMA donde empieza el recorte 4:3 (0..120; 60 = centro)
    int32_t    fuerza;      // fuerza de los pines 0..3 (con resistencias: 3 = la de menor resistencia interna)
    int32_t    fuerza_1c;   // en el modo 1 cable (0 = la más débil: el pin da ~1 V sobre los 75 ohm)
    int32_t    audio, vol;
} tv_guardado_t;
#define TV_VERSION 2

static tv_guardado_t s_g;
static ntsc_t *s_n;                       // tabla del DAC y niveles (RAM interna, ~5 KB)
static uint8_t *s_fb;                     // el cuadro NTSC (PSRAM): lo lee el DMA sin parar
static uint16_t *s_carta;                 // carta de ajuste 360x240 RGB565 (PSRAM)
static lcd_cam_dev_t *s_lcd;
static gdma_channel_handle_t s_dma;
static gdma_link_list_handle_t s_link;
static uint32_t s_apll_real;
static size_t s_nodos;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_libre, s_mux;
static const uint16_t *volatile s_img;
static volatile int s_img_w, s_img_h;
static volatile bool s_rehacer = true;
static volatile float s_ms_conv;
static volatile uint32_t s_cuadros;
static bool s_ok;
static char s_prueba[640] = "null";
static uint8_t *s_cap;                    // la última captura de la autoprueba
static volatile bool s_probando;

// ---------------- configuración guardada (NVS compartida con Cerebro: espacio propio "tv", nunca se borra) --------
static void cfg_default(void)
{
    memset(&s_g, 0, sizeof(s_g));
    s_g.version = TV_VERSION;
    ntsc_cfg_default(&s_g.ntsc);
    s_g.modo = 0;
    s_g.x0 = 60;
    s_g.fuerza = 3;
    s_g.fuerza_1c = 0;
    s_g.audio = 1;
    s_g.vol = 35;
}

static void cfg_cargar(void)
{
    cfg_default();
    nvs_handle_t h;
    if (nvs_open("tv", NVS_READONLY, &h) != ESP_OK) return;
    tv_guardado_t g;
    size_t len = sizeof(g);
    if (nvs_get_blob(h, "cfg", &g, &len) == ESP_OK && len == sizeof(g) && g.version == TV_VERSION) s_g = g;
    nvs_close(h);
}

static void cfg_guardar(void)
{
    nvs_handle_t h;
    if (nvs_open("tv", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &s_g, sizeof(s_g));
    nvs_commit(h);
    nvs_close(h);
}

// ---------------- motor: LCD_CAM i80 continuo + DMA en anillo ----------------
static void pines_video(void)
{
    int fuerza = s_g.ntsc.un_cable ? s_g.fuerza_1c : s_g.fuerza;
    if (fuerza < 0) fuerza = 0;
    if (fuerza > 3) fuerza = 3;
    for (int i = 0; i < 6; i++) {
        int p = TV_PIN_DAC0 + i;
        gpio_func_sel(p, PIN_FUNC_GPIO);
        // la conexión por la matriz también habilita la salida del pin
        esp_rom_gpio_connect_out_signal(p, soc_lcd_i80_signals[0].data_sigs[i], false, false);
        gpio_set_drive_capability(p, (gpio_drive_cap_t)fuerza);
    }
}

static esp_err_t motor_start(void)
{
    s_lcd = LCD_LL_GET_HW(0);
    PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[0].module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(0, true);
            lcd_ll_reset_register(0);
        }
    }
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(s_lcd, true);
    }
    // reloj exacto de 4×fsc desde el APLL (nadie más lo usa en este firmware: el sonido va con el cristal)
    ESP_RETURN_ON_ERROR(esp_clk_tree_enable_src(SOC_MOD_CLK_APLL, true), TAG, "no pude encender el APLL");
    ESP_RETURN_ON_ERROR(esp_clk_tree_src_set_freq_hz(SOC_MOD_CLK_APLL, APLL_HZ, &s_apll_real), TAG, "APLL");
    PERIPH_RCC_ATOMIC() {
        lcd_ll_select_clk_src(0, LCD_CLK_SRC_APLL);
        lcd_ll_set_group_clock_coeff(0, 2, 0, 0);        // LCD_CLK = APLL / 2 = 57,27 MHz
    }
    lcd_ll_set_pixel_clock_prescale(s_lcd, 4);            // PCLK = LCD_CLK / 4 = 14,318 MHz
    lcd_ll_set_clock_idle_level(s_lcd, false);
    lcd_ll_set_pixel_clock_edge(s_lcd, false);
    lcd_ll_reset(s_lcd);
    lcd_ll_fifo_reset(s_lcd);
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(s_lcd, LCD_LL_EVENT_I80, false);
    }
    lcd_ll_clear_interrupt_status(s_lcd, UINT32_MAX);
    // modo i80 sólo con fase de datos, sin borrados, y la duración la manda el DMA (que nunca termina: anillo)
    lcd_ll_enable_rgb_mode(s_lcd, false);
    lcd_ll_enable_color_convert(s_lcd, false);
    lcd_ll_set_dma_read_stride(s_lcd, 8);
    lcd_ll_set_data_wire_width(s_lcd, 8);
    lcd_ll_set_phase_cycles(s_lcd, 0, 0, 1);
    lcd_ll_enable_output_always_on(s_lcd, true);
    lcd_ll_set_blank_cycles(s_lcd, 0, 0);

    gdma_channel_alloc_config_t dc = { 0 };
    ESP_RETURN_ON_ERROR(gdma_new_axi_channel(&dc, &s_dma, NULL), TAG, "canal DMA");
    ESP_RETURN_ON_ERROR(gdma_connect(s_dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)), TAG, "DMA → LCD");
    gdma_strategy_config_t st = { .owner_check = false, .auto_update_desc = false, .eof_till_data_popped = false };
    gdma_apply_strategy(s_dma, &st);
    gdma_transfer_config_t tc = { .max_data_burst_size = 32, .access_ext_mem = true };
    ESP_RETURN_ON_ERROR(gdma_config_transfer(s_dma, &tc), TAG, "ráfagas DMA");
    if (gdma_set_priority(s_dma, 1) != ESP_OK) ESP_LOGW(TAG, "no pude subir la prioridad del DMA");
    gdma_channel_alignment_info_t al = { 0 };
    gdma_get_channel_alignment_constraints(s_dma, &al);
    size_t alin = al.ext_no_enc_mem_alignment ? al.ext_no_enc_mem_alignment : 1;
    s_nodos = esp_dma_calculate_node_count(NTSC_FB, alin, 4095);
    gdma_link_list_config_t lc = { .num_items = s_nodos, .item_alignment = 8, .flags = { .check_owner = false } };
    ESP_RETURN_ON_ERROR(gdma_new_link_list(&lc, &s_link), TAG, "lista DMA");
    gdma_buffer_mount_config_t mc = {
        .buffer = s_fb, .buffer_alignment = alin, .length = NTSC_FB,
        .flags = { .mark_eof = 0, .mark_final = GDMA_FINAL_LINK_TO_HEAD, .bypass_buffer_size_align_check = 1 },
    };
    ESP_RETURN_ON_ERROR(gdma_link_mount_buffers(s_link, 0, &mc, 1, NULL), TAG, "montar el cuadro");

    pines_video();
    lcd_ll_fifo_reset(s_lcd);
    gdma_reset(s_dma);
    gdma_start(s_dma, gdma_link_get_head_addr(s_link));
    esp_rom_delay_us(1);
    lcd_ll_start(s_lcd);
    ESP_LOGI(TAG, "señal NTSC en marcha: APLL %lu Hz → píxel %.1f Hz, %u nodos DMA", (unsigned long)s_apll_real,
             s_apll_real / 8.0, (unsigned)s_nodos);
    return ESP_OK;
}

// ---------------- contenido ----------------
static void sync_cache(size_t desde, size_t largo)
{
    esp_cache_msync(s_fb + desde, largo, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static void reconstruir(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    ntsc_init(s_n, &s_g.ntsc);
    ntsc_cuadro_base(s_n, s_fb);
    if (s_g.modo == 1) ntsc_imagen_rgb565(s_n, s_fb, s_carta, NTSC_IMG_PX, NTSC_IMG_H, 0, 0, NTSC_IMG_H);
    else if (s_g.modo == 3) ntsc_imagen_plana(s_n, s_fb, s_g.ntsc.blanco_ire);
    sync_cache(0, NTSC_FB);
    if (s_ok) pines_video();
    s_rehacer = false;
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "cuadro rehecho: modo %d, %s, vmax %.3f V, códigos sync %d blank %d negro %d blanco %d",
             (int)s_g.modo, s_g.ntsc.un_cable ? "1 cable" : "resistencias", s_n->vmax, ntsc_q(s_n, s_n->l_sync),
             ntsc_q(s_n, s_n->l_blank), ntsc_q(s_n, s_n->l_black), ntsc_q(s_n, s_n->l_white));
}

static void tv_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (s_rehacer && !s_probando) reconstruir();
        const uint16_t *img = s_img;
        if (!img) continue;
        if (s_g.modo == 0 && !s_rehacer && !s_probando) {
            int64_t t0 = esp_timer_get_time();
            xSemaphoreTake(s_mux, portMAX_DELAY);
            ntsc_imagen_rgb565(s_n, s_fb, img, s_img_w, s_img_h, s_g.x0, 0, NTSC_IMG_H);
            s_img = NULL;
            xSemaphoreGive(s_libre);                        // BIOMA ya puede reescribir su cuadro chico
            sync_cache((size_t)NTSC_IMG_Y0 * NTSC_W, (size_t)NTSC_IMG_H * NTSC_W);
            xSemaphoreGive(s_mux);
            s_ms_conv = (esp_timer_get_time() - t0) / 1000.0f;
            s_cuadros++;
        } else {
            s_img = NULL;
            xSemaphoreGive(s_libre);
        }
    }
}

void tv_cuadro_listo(const uint16_t *img, int ancho, int alto)
{
    if (!s_ok || s_g.modo != 0) return;
    if (xSemaphoreTake(s_libre, 0) != pdTRUE) return;        // la tele sigue con el anterior: éste se salta
    s_img_w = ancho;
    s_img_h = alto;
    s_img = img;
    if (s_task) xTaskNotifyGive(s_task);
}

void tv_esperar_libre(void)
{
    if (!s_ok) return;
    if (xSemaphoreTake(s_libre, pdMS_TO_TICKS(100)) == pdTRUE) xSemaphoreGive(s_libre);
}

void tv_evento_evolucion(bool aceptado)
{
    if (aceptado) tv_audio_campana();
}

// ---------------- autoprueba: el P4 graba su propia salida ----------------
static double medir_pclk(void)
{
    pcnt_unit_handle_t u = NULL;
    pcnt_channel_handle_t ch = NULL;
    pcnt_unit_config_t uc = { .low_limit = -1, .high_limit = 20000, .flags = { .accum_count = 1 } };
    double f = -1;
    if (pcnt_new_unit(&uc, &u) != ESP_OK) return -1;
    pcnt_chan_config_t cc = { .edge_gpio_num = TV_PIN_PRUEBA, .level_gpio_num = -1 };
    if (pcnt_new_channel(u, &cc, &ch) == ESP_OK &&
        pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD) == ESP_OK &&
        pcnt_unit_add_watch_point(u, 20000) == ESP_OK && pcnt_unit_enable(u) == ESP_OK) {
        pcnt_unit_clear_count(u);
        int64_t t0 = esp_timer_get_time();
        pcnt_unit_start(u);
        vTaskDelay(pdMS_TO_TICKS(500));
        pcnt_unit_stop(u);
        int64_t t1 = esp_timer_get_time();
        int c = 0;
        pcnt_unit_get_count(u, &c);
        f = c / ((t1 - t0) / 1e6);
        pcnt_unit_disable(u);
    }
    if (ch) pcnt_del_channel(ch);
    pcnt_del_unit(u);
    return f;
}

// ¿la captura aparece tal cual en el cuadro? (en algún desfase; el cuadro es un anillo)
static int comparar(const uint8_t *cap, size_t n, uint8_t mascara, int *desfase)
{
    int mejor = 0;
    *desfase = -1;
    for (size_t off = 0; off < NTSC_FB; off++) {
        size_t k = 0;
        while (k < 64 && (cap[k] & mascara) == s_fb[(off + k) % NTSC_FB]) k++;
        if (k < 64) continue;
        // en zonas parejas (negro, sincronismo) muchos desfases calzan los primeros 64: primero un muestreo ralo
        for (k = 0; k < n; k += 509) if ((cap[k] & mascara) != s_fb[(off + k) % NTSC_FB]) break;
        if (k < n) continue;
        size_t ok = 0;
        for (size_t i = 0; i < n; i++) ok += (cap[i] & mascara) == s_fb[(off + i) % NTSC_FB];
        if ((int)ok > mejor) { mejor = (int)ok; *desfase = (int)off; }
        if (ok == n) break;
    }
    return mejor;
}

static void prueba(void)
{
    s_probando = true;
    xSemaphoreTake(s_mux, portMAX_DELAY);                  // el cuadro queda quieto mientras se compara
    gpio_func_sel(TV_PIN_PRUEBA, PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(TV_PIN_PRUEBA, soc_lcd_i80_signals[0].wr_sig, false, false);
    double f = medir_pclk();

    if (!s_cap) s_cap = heap_caps_aligned_calloc(64, 1, 65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    int res[2][CAP_VECES];
    int desf[2][CAP_VECES];
    memset(res, 0, sizeof(res));
    memset(desf, 0xff, sizeof(desf));
    const char *error = "";
    uint8_t mascara = s_n->ncodigos - 1;
    for (int e = 0; e < 2 && s_cap; e++) {                 // flanco de bajada y de subida
        parlio_rx_unit_handle_t rx = NULL;
        parlio_rx_delimiter_handle_t del = NULL;
        parlio_rx_unit_config_t rc = {
            .trans_queue_depth = 2, .max_recv_size = 65536, .data_width = 8,
            .clk_src = PARLIO_CLK_SRC_EXTERNAL, .ext_clk_freq_hz = NTSC_FS_HZ, .exp_clk_freq_hz = NTSC_FS_HZ,
            .clk_in_gpio_num = TV_PIN_PRUEBA, .clk_out_gpio_num = -1, .valid_gpio_num = -1,
            .data_gpio_nums = { 16, 17, 18, 19, 20, 21, 22, 23, -1, -1, -1, -1, -1, -1, -1, -1 },
            .flags = { .free_clk = 1 },
        };
        if (parlio_new_rx_unit(&rc, &rx) != ESP_OK) { error = "PARLIO RX"; break; }
        parlio_rx_soft_delimiter_config_t sc = {
            .sample_edge = e == 0 ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS,
            .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB, .eof_data_len = CAP_LEN, .timeout_ticks = 0,
        };
        if (parlio_new_rx_soft_delimiter(&sc, &del) == ESP_OK && parlio_rx_unit_enable(rx, true) == ESP_OK) {
            parlio_receive_config_t rcv = { .delimiter = del };
            for (int v = 0; v < CAP_VECES; v++) {
                memset(s_cap, 0xAA, 65536);
                esp_cache_msync(s_cap, 65536, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                if (parlio_rx_unit_receive(rx, s_cap, 65536, &rcv) != ESP_OK) { error = "receive"; break; }
                parlio_rx_soft_delimiter_start_stop(rx, del, true);
                esp_err_t w = parlio_rx_unit_wait_all_done(rx, 1000);
                parlio_rx_soft_delimiter_start_stop(rx, del, false);
                if (w != ESP_OK) { error = "sin datos (¿no llega el reloj?)"; break; }
                esp_cache_msync(s_cap, 65536, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                res[e][v] = comparar(s_cap, CAP_LEN, mascara, &desf[e][v]);
                vTaskDelay(pdMS_TO_TICKS(7));              // otra parte del cuadro la próxima vez
            }
            parlio_rx_unit_disable(rx);
        } else error = "delimitador";
        if (del) parlio_del_rx_delimiter(del);
        parlio_del_rx_unit(rx);
        if (e == 0 && res[0][0] == CAP_LEN) break;         // con el primer flanco ya calza: no hace falta el otro
    }
    // IO33 vuelve a quedar como entrada sin nada
    esp_rom_gpio_connect_out_signal(TV_PIN_PRUEBA, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(TV_PIN_PRUEBA, GPIO_MODE_INPUT);
    xSemaphoreGive(s_mux);
    s_probando = false;

    char *p = s_prueba;
    size_t cap = sizeof(s_prueba);
    int n = snprintf(p, cap, "{\"pclk_hz\":%.1f,\"pclk_esperado_hz\":%.1f,\"error\":\"%s\",\"muestras_por_captura\":%d,\"flanco_bajada\":[",
                     f, s_apll_real / 8.0, error, CAP_LEN);
    for (int e = 0; e < 2; e++) {
        for (int v = 0; v < CAP_VECES && n < (int)cap - 40; v++)
            n += snprintf(p + n, cap - n, "%s[%d,%d]", v ? "," : "", res[e][v], desf[e][v]);
        n += snprintf(p + n, cap - n, e == 0 ? "],\"flanco_subida\":[" : "]}");
    }
    ESP_LOGI(TAG, "autoprueba: %s", s_prueba);
}

// ---------------- web ----------------
static void estado_json(char *b, size_t cap)
{
    const ntsc_cfg_t *c = &s_g.ntsc;
    snprintf(b, cap,
             "{\"ok\":%s,\"modo\":%d,\"uncable\":%d,\"fuerza\":%d,\"fuerza_1c\":%d,\"bits\":%d,\"r\":[%.0f,%.0f,%.0f,%.0f,%.0f,%.0f],"
             "\"setup\":%.1f,\"croma\":%.2f,\"filtro\":%d,\"x0\":%d,\"audio\":%d,\"vol\":%d,\"apll_hz\":%lu,\"pclk_hz\":%.1f,"
             "\"vmax\":%.3f,\"codigos\":{\"sync\":%d,\"blank\":%d,\"negro\":%d,\"blanco\":%d},\"cuadros\":%lu,\"ms_conversion\":%.2f,"
             "\"nodos_dma\":%u,\"prueba\":%s}",
             s_ok ? "true" : "false", (int)s_g.modo, c->un_cable, (int)s_g.fuerza, (int)s_g.fuerza_1c, c->bits,
             c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->setup_ire, c->croma, c->filtro_croma, (int)s_g.x0,
             (int)s_g.audio, (int)s_g.vol, (unsigned long)s_apll_real, s_apll_real / 8.0, s_n ? s_n->vmax : 0,
             s_n ? ntsc_q(s_n, s_n->l_sync) : 0, s_n ? ntsc_q(s_n, s_n->l_blank) : 0, s_n ? ntsc_q(s_n, s_n->l_black) : 0,
             s_n ? ntsc_q(s_n, s_n->l_white) : 0, (unsigned long)s_cuadros, s_ms_conv, (unsigned)s_nodos, s_prueba);
}

static bool q_int(const char *q, const char *k, int32_t *v, int lo, int hi)
{
    char t[24];
    if (httpd_query_key_value(q, k, t, sizeof(t)) != ESP_OK) return false;
    int x = atoi(t);
    *v = x < lo ? lo : x > hi ? hi : x;
    return true;
}

static bool q_float(const char *q, const char *k, float *v, float lo, float hi)
{
    char t[24];
    if (httpd_query_key_value(q, k, t, sizeof(t)) != ESP_OK) return false;
    float x = strtof(t, NULL);
    *v = x < lo ? lo : x > hi ? hi : x;
    return true;
}

static esp_err_t h_tv(httpd_req_t *r)
{
    char q[400];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        bool cambio = false, audio = false;
        int32_t v;
        if (q_int(q, "modo", &s_g.modo, 0, 3)) cambio = true;
        if (q_int(q, "x0", &s_g.x0, 0, 120)) cambio = true;
        if (q_int(q, "fuerza", &s_g.fuerza, 0, 3)) cambio = true;
        if (q_int(q, "fuerza_1c", &s_g.fuerza_1c, 0, 3)) cambio = true;
        if (q_int(q, "uncable", &v, 0, 1)) { s_g.ntsc.un_cable = v; cambio = true; }
        if (q_int(q, "filtro", &v, 0, 1)) { s_g.ntsc.filtro_croma = v; cambio = true; }
        if (q_float(q, "setup", &s_g.ntsc.setup_ire, 0, 10)) cambio = true;
        if (q_float(q, "croma", &s_g.ntsc.croma, 0, 2)) cambio = true;
        if (q_float(q, "valto", &s_g.ntsc.v_alto, 0.3f, 3.3f)) cambio = true;
        if (q_int(q, "audio", &s_g.audio, 0, 1)) audio = true;
        if (q_int(q, "vol", &s_g.vol, 0, 100)) audio = true;
        char t[96];
        if (httpd_query_key_value(q, "r", t, sizeof(t)) == ESP_OK) {       // r=10000,4700,2200,1000,510,270
            char *s = t;
            int k = 0;
            while (*s && k < 6) {
                float x = strtof(s, &s);
                if (x >= 10 && x <= 1e6) s_g.ntsc.r[k] = x;
                k++;
                while (*s == ',' || *s == ' ') s++;
            }
            cambio = true;
        }
        if (httpd_query_key_value(q, "defecto", t, sizeof(t)) == ESP_OK) { cfg_default(); cambio = audio = true; }
        if (cambio) { s_rehacer = true; if (s_task) xTaskNotifyGive(s_task); }
        if (audio) tv_audio_set(s_g.audio, s_g.vol);
        if (cambio || audio) cfg_guardar();
    }
    char b[1400];
    estado_json(b, sizeof(b));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, b);
}

static esp_err_t h_prueba(httpd_req_t *r)
{
    if (s_ok) prueba();
    return h_tv(r);
}

static esp_err_t h_captura(httpd_req_t *r)
{
    if (!s_cap) return httpd_resp_send_404(r);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)s_cap, CAP_LEN);
}

static esp_err_t h_cuadro(httpd_req_t *r)                 // el cuadro completo tal como está en memoria
{
    if (!s_fb) return httpd_resp_send_404(r);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)s_fb, NTSC_FB);
}

void tv_registrar(httpd_handle_t s)
{
    httpd_uri_t u[] = {
        { .uri = "/api/tv", .method = HTTP_GET, .handler = h_tv },
        { .uri = "/api/tv/prueba", .method = HTTP_GET, .handler = h_prueba },
        { .uri = "/api/tv/captura.bin", .method = HTTP_GET, .handler = h_captura },
        { .uri = "/api/tv/cuadro.bin", .method = HTTP_GET, .handler = h_cuadro },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(s, &u[i]);
}

// ---------------- arranque ----------------
void tv_start(void)
{
    cfg_cargar();
    s_n = heap_caps_calloc(1, sizeof(ntsc_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_n) s_n = heap_caps_calloc(1, sizeof(ntsc_t), MALLOC_CAP_SPIRAM);
    s_fb = heap_caps_aligned_calloc(64, 1, P4_ALIGN(NTSC_FB, 64), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_carta = heap_caps_calloc(NTSC_IMG_PX * NTSC_IMG_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_libre = xSemaphoreCreateBinary();
    s_mux = xSemaphoreCreateMutex();
    if (!s_n || !s_fb || !s_carta || !s_libre || !s_mux) { ESP_LOGE(TAG, "falta memoria para la tele"); return; }
    xSemaphoreGive(s_libre);
    ntsc_carta_rgb565(s_carta);
    reconstruir();
    if (motor_start() != ESP_OK) { ESP_LOGE(TAG, "la salida de video no arrancó"); return; }
    // conversión de los cuadros de BIOMA: por encima de la simulación (8) para que no se atrase, bajo el JPEG (10)
    // pila holgada: ntsc_imagen_rgb565 usa ~3,6 KB de arreglos locales por línea (con 4 KB se desbordaba)
    if (xTaskCreatePinnedToCore(tv_task, "tele", 10240, NULL, 9, &s_task, 1) != pdPASS) { ESP_LOGE(TAG, "sin tarea de la tele"); return; }
    s_ok = true;                  // recién ahora BIOMA puede mandar cuadros (antes, s_task era NULL)
    tv_audio_start();
    tv_audio_set(s_g.audio, s_g.vol);
}
