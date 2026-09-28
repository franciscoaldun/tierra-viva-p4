// lcd.c — BIOMA en la pantalla táctil de 2,8" (ILI9341 240x320 SPI + táctil XPT2046), enchufada directo en la
// protoboard: sus 14 pines caen en los 14 primeros pines del header DERECHO del P4, en el mismo orden, sin cables.
//
//   pantalla:  VCC  GND  CS  RESET DC  SDI  SCK LED SDO  T_CLK T_CS T_DIN T_DO T_IRQ
//   P4:        IO15 IO14 IO13 IO12 IO11 IO10 IO9 IO6 IO5  IO4   IO3  IO2   IO54 IO53
//
// Ningún lugar del P4 tiene 3,3 V y GND en ese orden, así que el P4 FABRICA la alimentación: IO15 queda en alto
// fijo (VCC) e IO14 en bajo fijo (GND), con la fuerza máxima del pin. Un pin entrega ~40 mA: alcanza para la lógica de
// la pantalla y el táctil; la luz de fondo va por su propio pin (LED, IO6) con PWM (parte al 100 %; tocar la mitad derecha la baja).
// Primero se enciende la alimentación y recién después se tocan las señales (así no se alimenta por las señales).
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "app.h"
#include "tierra_app.h"
#include "lcd.h"

static const char *TAG = "pantalla";

#define P_VCC   15
#define P_GND   14
#define P_CS    13
#define P_RST   12
#define P_DC    11
#define P_MOSI  10
#define P_SCK   9
#define P_LED   6
#define P_MISO  5
#define T_CLK   4
#define T_CS    3
#define T_DIN   2
#define T_DO    54
#define T_IRQ   53

#define ANCHO   320
#define ALTO    240
#define FRANJA  12                                   // filas por envío (2 búferes de 320x12 en RAM interna)
#define CUADRO  (ANCHO * ALTO * 2)
// recorte del centro de BIOMA que va a la pantalla: 342x256 desde (69,7), escalado x15/16 por el PPA = 320x240
// (el PPA escala en dieciseisavos: 15/16 es exacto y casi 1:1, así que sale nítido)
#define REC_X   69
#define REC_Y   7
#define REC_W   342
#define REC_H   256

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_buf[2];
static SemaphoreHandle_t s_libres;
static TaskHandle_t s_task;
static volatile float s_fps, s_fps_bioma, s_t_mezcla, s_t_envio;
static int s_spi_hz;
// PPA: el recorte a 320x240 (SRM) y la mezcla entre el cuadro anterior y el nuevo (blend), para que la pantalla
// muestre ~30 cuadros/s aunque BIOMA entregue ~13: entre dos cuadros de la vida se dibujan los intermedios.
static ppa_client_handle_t s_ppa_rec, s_ppa_mez;
#define NCQ 6
static uint16_t *s_cq[NCQ], *s_mix[2], *s_spi[2];    // cuadros 320x240 recortados, 2 mezclas y 2 listos para el SPI (PSRAM)
static bool s_ppa_voltea;                            // el PPA deja los bytes en el orden de la pantalla (probado al arrancar)
static volatile int s_uso[3] = { -1, -1, -1 };       // cuadros que la pantalla está leyendo (mezcla y envío)
static SemaphoreHandle_t s_mez_lista;
static volatile int s_ia = -1, s_ib = -1;            // cuadro anterior y más nuevo
static volatile int64_t s_tb;                        // cuándo llegó el más nuevo
static volatile float s_periodo = 75000.0f;          // µs entre cuadros de BIOMA (promedio)
static volatile uint32_t s_seq;
static SemaphoreHandle_t s_mux;
static volatile bool s_ventana_ok;
// color: tablas por canal que ya traen el byte alto primero (el orden que pide la pantalla) y una curva gamma
// que levanta los medios tonos (la luz de fondo de esta pantalla es débil)
static uint16_t s_lr[32], s_lg[64], s_lb[32];
static volatile float s_gamma = 1.0f;               // 1 = colores tal cual (y así todo lo hace el PPA, sin CPU)
static volatile uint32_t s_cuadros, s_toques;
static volatile int s_tx = -1, s_ty = -1;
static volatile int s_brillo = 100;
static bool s_ok;

static bool IRAM_ATTR on_listo(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_libres, &hp);
    return hp == pdTRUE;
}

static void cmd(uint8_t c, const uint8_t *p, int n) { esp_lcd_panel_io_tx_param(s_io, c, p, n); }

static void pines_salida(int pin, int nivel)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    gpio_set_level(pin, nivel);
}

static void brillo(int pct)
{
    s_brillo = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)(1023 * s_brillo / 100));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint16_t bswap(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

static void tablas_color(float g)
{
    for (int i = 0; i < 32; i++) {
        int v = (int)lroundf(31.0f * powf(i / 31.0f, g));
        s_lr[i] = bswap((uint16_t)(v << 11));
        s_lb[i] = bswap((uint16_t)v);
    }
    for (int i = 0; i < 64; i++) s_lg[i] = bswap((uint16_t)((int)lroundf(63.0f * powf(i / 63.0f, g)) << 5));
    s_gamma = g;
}

// ---------------- táctil XPT2046 (lento: se lee "a mano" por sus 4 pines) ----------------
static int xpt_leer(uint8_t orden)
{
    gpio_set_level(T_CS, 0);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(T_DIN, (orden >> i) & 1);
        gpio_set_level(T_CLK, 1); esp_rom_delay_us(1); gpio_set_level(T_CLK, 0); esp_rom_delay_us(1);
    }
    int v = 0;
    for (int i = 0; i < 16; i++) {
        gpio_set_level(T_CLK, 1); esp_rom_delay_us(1);
        v = (v << 1) | gpio_get_level(T_DO);
        gpio_set_level(T_CLK, 0); esp_rom_delay_us(1);
    }
    gpio_set_level(T_CS, 1);
    return (v >> 4) & 0xFFF;                          // 12 bits útiles
}

// promedio de 8 lecturas mientras el dedo sigue apoyado (false si se soltó a la mitad)
static bool leer_crudo(int *rx, int *ry)
{
    if (gpio_get_level(T_IRQ)) return false;
    int sx = 0, sy = 0;
    for (int i = 0; i < 8; i++) { sx += xpt_leer(0xD0); sy += xpt_leer(0x90); }
    if (gpio_get_level(T_IRQ)) return false;
    *rx = sx / 8; *ry = sy / 8;
    return true;
}

// ---------------- calibración (4 cruces; queda guardada en la NVS, espacio "lcd") ----------------
typedef struct { uint32_t magia; int32_t swap, xa, xb, ya, yb; } cal_t;   // crudo en x=20/x=300 y en y=20/y=220
static cal_t s_cal;
static bool s_cal_ok;
static volatile bool s_calibrar;
#define CAL_MAGIA 0xCA11B8A7

static void cal_guardar(void)
{
    nvs_handle_t h;
    if (nvs_open("lcd", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cal", &s_cal, sizeof(s_cal));
    nvs_commit(h);
    nvs_close(h);
}

static void cal_cargar(void)
{
    nvs_handle_t h;
    size_t n = sizeof(s_cal);
    if (nvs_open("lcd", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "cal", &s_cal, &n) == ESP_OK && n == sizeof(s_cal) && s_cal.magia == CAL_MAGIA) s_cal_ok = true;
        nvs_close(h);
    }
}

static void mapear(int rx, int ry, int *x, int *y)
{
    int a = s_cal.swap ? ry : rx, b = s_cal.swap ? rx : ry;
    int sx = 20 + (a - s_cal.xa) * 280 / (s_cal.xb - s_cal.xa ? s_cal.xb - s_cal.xa : 1);
    int sy = 20 + (b - s_cal.ya) * 200 / (s_cal.yb - s_cal.ya ? s_cal.yb - s_cal.ya : 1);
    *x = sx < 0 ? 0 : sx >= ANCHO ? ANCHO - 1 : sx;
    *y = sy < 0 ? 0 : sy >= ALTO ? ALTO - 1 : sy;
}

// esquina de arriba a la izquierda: reloj sí/no (y el dedo 4 s ahí = calibrar de nuevo)
// esquina de arriba a la derecha: luz · todo lo demás: el dedo atrae a la colonia (y se puede arrastrar)
#define ESQ_W 64
#define ESQ_H 48
static volatile int s_diag_irq = -1, s_diag_x = -1, s_diag_y = -1, s_diag_z = -1;
static void tactil(void)
{
    static int64_t t_diag;
    if (esp_timer_get_time() - t_diag > 1000000) {       // diagnóstico: lectura cruda cada 1 s, aunque nadie toque
        t_diag = esp_timer_get_time();
        s_diag_irq = gpio_get_level(T_IRQ);
        s_diag_x = xpt_leer(0xD0); s_diag_y = xpt_leer(0x90); s_diag_z = xpt_leer(0xB0);
    }
    static bool apretado = false, en_esquina = false;
    static int64_t desde = 0;
    bool ahora = gpio_get_level(T_IRQ) == 0;
    int64_t t = esp_timer_get_time();
    int rx, ry;
    if (ahora && leer_crudo(&rx, &ry)) {
        int x = rx * ANCHO / 4096, y = ry * ALTO / 4096;
        if (s_cal_ok) mapear(rx, ry, &x, &y);
        s_tx = x; s_ty = y;
        if (!apretado) {
            desde = t;
            s_toques++;
            en_esquina = (y < ESQ_H && (x < ESQ_W || x >= ANCHO - ESQ_W)) || (y >= ALTO - ESQ_H && x < ESQ_W);
            ESP_LOGI(TAG, "toque %lu en (%d, %d) · crudo (%d, %d)", (unsigned long)s_toques, x, y, rx, ry);
            if (en_esquina && y >= ALTO - ESQ_H) bioma_set_escena(!bioma_escena());    // abajo a la izquierda: BIOMA <-> TIERRA VIVA
            else if (en_esquina && x < ESQ_W) { static bool reloj = true; reloj = !reloj; bioma_set_clock(reloj); }
            else if (en_esquina) brillo(s_brillo >= 100 ? 40 : s_brillo + 20);
        }
        if (!en_esquina) {
            if (bioma_escena() == 1) bioma_toque(TIERRA_LCD_X + x, TIERRA_LCD_Y + y);
            else bioma_toque(REC_X + x * 16 / 15, REC_Y + y * 16 / 15);
        }
        if (en_esquina && x < ESQ_W && y < ESQ_H && desde && t - desde > 4000000) { desde = 0; s_calibrar = true; }
    }
    apretado = ahora;
}

// ---------------- dibujo ----------------
static void ventana_xy(int x0, int x1, int y0, int y1)
{
    uint8_t c[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    cmd(0x2A, c, 4);
    uint8_t p[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    cmd(0x2B, p, 4);
}

static void ventana(int y0, int y1) { ventana_xy(0, ANCHO - 1, y0, y1); }

static void rect(int x0, int y0, int w, int h, uint16_t color)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > ANCHO) w = ANCHO - x0;
    if (y0 + h > ALTO) h = ALTO - y0;
    if (w <= 0 || h <= 0) return;
    uint16_t px = (uint16_t)((color << 8) | (color >> 8));
    int total = w * h, lote = ANCHO * FRANJA;
    xSemaphoreTake(s_libres, portMAX_DELAY);
    xSemaphoreTake(s_libres, portMAX_DELAY);            // los dos búferes libres: se usa el 0 sin pisar envíos
    for (int i = 0; i < (total < lote ? total : lote); i++) s_buf[0][i] = px;
    xSemaphoreGive(s_libres);
    xSemaphoreGive(s_libres);
    ventana_xy(x0, x0 + w - 1, y0, y0 + h - 1);
    s_ventana_ok = false;
    bool primero = true;
    while (total > 0) {
        int n = total < lote ? total : lote;
        xSemaphoreTake(s_libres, portMAX_DELAY);
        esp_lcd_panel_io_tx_color(s_io, primero ? 0x2C : 0x3C, s_buf[0], n * 2);   // 0x3C: seguir escribiendo
        primero = false;
        total -= n;
    }
    xSemaphoreTake(s_libres, portMAX_DELAY); xSemaphoreGive(s_libres);
}

static void cruz(int x, int y, uint16_t c)
{
    rect(x - 12, y, 25, 1, c);
    rect(x, y - 12, 1, 25, c);
    rect(x - 2, y - 2, 5, 5, c);
}

static void calibrar(void)
{
    static const int px[4] = { 20, 300, 300, 20 }, py[4] = { 20, 20, 220, 220 };
    int rx[4], ry[4];
    ESP_LOGW(TAG, "CALIBRACIÓN: toca el centro de cada cruz blanca (4 en total)");
    for (int i = 0; i < 4; i++) {
        rect(0, 0, ANCHO, ALTO, 0x0000);
        cruz(px[i], py[i], 0xFFFF);
        int64_t t0 = esp_timer_get_time();
        bool listo = false;
        while (!listo) {
            if (esp_timer_get_time() - t0 > 600000000LL) { ESP_LOGW(TAG, "calibración cancelada (nadie tocó)"); return; }
            vTaskDelay(pdMS_TO_TICKS(20));
            if (gpio_get_level(T_IRQ) == 0) {
                vTaskDelay(pdMS_TO_TICKS(120));          // que el dedo se asiente
                listo = leer_crudo(&rx[i], &ry[i]);
            }
        }
        ESP_LOGI(TAG, "cruz %d (%d,%d): crudo (%d, %d)", i + 1, px[i], py[i], rx[i], ry[i]);
        rect(0, 0, ANCHO, ALTO, 0x07E0);                 // verde: registrado
        while (gpio_get_level(T_IRQ) == 0) vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    // ¿qué eje crudo acompaña al eje x de la pantalla? el que más cambia entre izquierda y derecha
    int dx_rx = abs((rx[1] + rx[2]) - (rx[0] + rx[3])), dx_ry = abs((ry[1] + ry[2]) - (ry[0] + ry[3]));
    s_cal.swap = dx_ry > dx_rx;
    int *ax = s_cal.swap ? ry : rx, *ay = s_cal.swap ? rx : ry;
    s_cal.xa = (ax[0] + ax[3]) / 2; s_cal.xb = (ax[1] + ax[2]) / 2;
    s_cal.ya = (ay[0] + ay[1]) / 2; s_cal.yb = (ay[2] + ay[3]) / 2;
    s_cal.magia = CAL_MAGIA;
    s_cal_ok = true;
    cal_guardar();
    ESP_LOGW(TAG, "calibración guardada: ejes %s, x %d→%d, y %d→%d", s_cal.swap ? "cruzados" : "directos",
             (int)s_cal.xa, (int)s_cal.xb, (int)s_cal.ya, (int)s_cal.yb);
}

// una operación del motor de mezcla del PPA. voltear = copiar "src" leyéndolo con los bytes al revés: el PPA toma
// cada píxel como si fuera el de bytes cambiados y lo escribe así, que es justo el orden que pide la pantalla
// (sin escalar ni mezclar no hay cuentas que lo estropeen: se verifica píxel por píxel al arrancar)
static esp_err_t mezcla(const uint16_t *bg, const uint16_t *fg, int alfa, bool voltear, uint16_t *out, ppa_trans_mode_t modo)
{
    ppa_blend_oper_config_t op = {
        .in_bg = { .buffer = bg, .pic_w = ANCHO, .pic_h = ALTO, .block_w = ANCHO, .block_h = ALTO, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .in_fg = { .buffer = fg, .pic_w = ANCHO, .pic_h = ALTO, .block_w = ANCHO, .block_h = ALTO, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .out = { .buffer = out, .buffer_size = CUADRO, .pic_w = ANCHO, .pic_h = ALTO, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .bg_byte_swap = voltear, .fg_byte_swap = voltear,
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .bg_alpha_fix_val = 255,
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .fg_alpha_fix_val = (uint32_t)(voltear ? 0 : alfa),
        .mode = modo,
    };
    return ppa_do_blend(s_ppa_mez, &op);
}

// cuadro ya volteado en PSRAM: el SPI lo lee directo por DMA, la CPU no toca ni un píxel
static void enviar_psram(const uint16_t *src)
{
    if (!s_ventana_ok) { ventana(0, ALTO - 1); s_ventana_ok = true; }
    xSemaphoreTake(s_libres, portMAX_DELAY);
    xSemaphoreTake(s_libres, portMAX_DELAY);            // nada más en vuelo
    esp_lcd_panel_io_tx_color(s_io, 0x2C, src, CUADRO);
    xSemaphoreTake(s_libres, portMAX_DELAY);            // lo devuelve el aviso de fin del último trozo
    xSemaphoreGive(s_libres);
    xSemaphoreGive(s_libres);
}

// envía un cuadro 320x240 completo: ventana fija, la primera franja con 0x2C y las demás con 0x3C (seguir escribiendo),
// así los envíos quedan en fila sin pausas entre franjas; mientras sale una franja, la CPU prepara la otra
static void enviar(const uint16_t *src)
{
    if (!s_ventana_ok) { ventana(0, ALTO - 1); s_ventana_ok = true; }
    int b = 0;
    for (int y0 = 0; y0 < ALTO; y0 += FRANJA) {
        xSemaphoreTake(s_libres, portMAX_DELAY);
        const uint32_t *o = (const uint32_t *)(src + y0 * ANCHO);
        uint32_t *d = (uint32_t *)s_buf[b];
        for (int i = 0; i < ANCHO * FRANJA / 2; i++) {         // de a 2 píxeles
            uint32_t c = o[i];
            uint32_t p0 = s_lr[(c >> 11) & 31] | s_lg[(c >> 5) & 63] | s_lb[c & 31];
            uint32_t p1 = s_lr[(c >> 27) & 31] | s_lg[(c >> 21) & 63] | s_lb[(c >> 16) & 31];
            d[i] = p0 | (p1 << 16);
        }
        esp_lcd_panel_io_tx_color(s_io, y0 ? 0x3C : 0x2C, s_buf[b], ANCHO * FRANJA * 2);
        b ^= 1;
    }
}

static bool IRAM_ATTR on_mezcla(ppa_client_handle_t c, ppa_event_data_t *e, void *u)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_mez_lista, &hp);
    return hp == pdTRUE;
}

// En tubería: mientras el SPI manda el cuadro k, el PPA ya mezcla el k+1 en la otra mezcla.
static void lcd_task(void *arg)
{
    int64_t t_ant = esp_timer_get_time(), t_log = t_ant;
    uint32_t seq_mostrado = 0;
    const uint16_t *listo = NULL;          // cuadro preparado en la vuelta anterior, para mandar en ésta
    int uso_listo = -1, m = 0;
    bool listo_ppa = false;
    while (1) {
        if (s_calibrar) {
            s_calibrar = false; calibrar(); seq_mostrado = 0; listo = NULL;
            s_uso[0] = s_uso[1] = s_uso[2] = -1;
            continue;
        }
        tactil();
        if (s_ib < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        int64_t t0 = esp_timer_get_time();
        // ¿qué va en el próximo cuadro? se calcula para cuando termine de salir el actual (~envío)
        xSemaphoreTake(s_mux, portMAX_DELAY);
        uint32_t seq = s_seq;
        int ia = s_ia, ib = s_ib;
        float a = (float)(t0 + (listo ? (int64_t)(s_t_envio * 1000.0f) : 0) - s_tb) / s_periodo;
        int alfa = a >= 1.0f ? 255 : a <= 0.0f ? 0 : (int)(a * 255.0f);
        bool mezclar = false, nuevo = true;
        if (alfa >= 250 || ia < 0 || ia == ib) {
            alfa = 255;
            nuevo = seq != seq_mostrado;
        } else mezclar = true;
        s_uso[2] = listo ? uso_listo : -1;         // el que sale en esta vuelta
        if (mezclar) { s_uso[0] = ia; s_uso[1] = ib; }
        else if (nuevo) { s_uso[0] = ib; s_uso[1] = -1; }
        xSemaphoreGive(s_mux);
        const uint16_t *sig = NULL;
        int uso_sig = -1;
        int pendiente = 0;
        bool ppa = s_ppa_voltea && s_gamma == 1.0f;
        if (ppa && (mezclar || nuevo)) {    // todo en el PPA: mezcla (si toca) y volteo, uno tras otro en su cola
            const uint16_t *verdad = s_cq[ib];
            if (mezclar && mezcla(s_cq[ia], s_cq[ib], alfa, false, s_mix[m], PPA_TRANS_MODE_NON_BLOCKING) == ESP_OK) {
                verdad = s_mix[m]; pendiente++;
            } else alfa = 255;
            if (mezcla(verdad, verdad, 0, true, s_spi[m], PPA_TRANS_MODE_NON_BLOCKING) == ESP_OK) { pendiente++; sig = s_spi[m]; m ^= 1; }
            else { while (pendiente) { xSemaphoreTake(s_mez_lista, portMAX_DELAY); pendiente--; } }
            uso_sig = -1;
        } else if (mezclar) {
            ppa_blend_oper_config_t op = {
                .in_bg = { .buffer = s_cq[ia], .pic_w = ANCHO, .pic_h = ALTO, .block_w = ANCHO, .block_h = ALTO, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
                .in_fg = { .buffer = s_cq[ib], .pic_w = ANCHO, .pic_h = ALTO, .block_w = ANCHO, .block_h = ALTO, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
                .out = { .buffer = s_mix[m], .buffer_size = CUADRO, .pic_w = ANCHO, .pic_h = ALTO, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
                .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .bg_alpha_fix_val = 255,
                .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .fg_alpha_fix_val = (uint32_t)alfa,
                .mode = PPA_TRANS_MODE_NON_BLOCKING,
            };
            if (ppa_do_blend(s_ppa_mez, &op) == ESP_OK) { pendiente = 1; sig = s_mix[m]; m ^= 1; }
            else { sig = s_cq[ib]; uso_sig = ib; alfa = 255; }
        } else if (nuevo) { sig = s_cq[ib]; uso_sig = ib; }
        int64_t t1 = esp_timer_get_time();
        if (listo) {                        // mientras el PPA mezcla, sale el cuadro anterior
            if (esp_ptr_external_ram(listo) && listo_ppa) enviar_psram(listo); else enviar(listo);
            s_uso[2] = -1;
            s_cuadros++;
            int64_t t = esp_timer_get_time();
            s_t_envio = s_t_envio * 0.9f + 0.1f * (t - t1) / 1000.0f;
            s_fps = s_fps * 0.9f + 0.1f * (1e6f / (float)(t - t_ant));
            t_ant = t;
        }
        int64_t t2 = esp_timer_get_time();
        while (pendiente) { xSemaphoreTake(s_mez_lista, portMAX_DELAY); pendiente--; }
        s_t_mezcla = s_t_mezcla * 0.9f + 0.1f * (esp_timer_get_time() - t2) / 1000.0f;   // lo que la mezcla NO alcanzó a esconder
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_uso[0] = s_uso[1] = -1;
        if (!mezclar && sig && uso_sig >= 0) s_uso[0] = uso_sig;   // se va a mandar directo: que nadie lo pise
        xSemaphoreGive(s_mux);
        listo = sig;
        uso_listo = uso_sig;
        listo_ppa = ppa;
        if (sig && alfa == 255) seq_mostrado = seq;
        if (!sig && !listo) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15));   // nada nuevo: esperar a BIOMA
        int64_t t = esp_timer_get_time();
        if (t - t_log > 10000000) {
            t_log = t;
            ESP_LOGI(TAG, "%.1f cuadros/s en pantalla (BIOMA %.1f) · espera de mezcla %.2f ms · enviar %.1f ms · SPI %.1f MHz · %s",
                     s_fps, s_fps_bioma, s_t_mezcla, s_t_envio, s_spi_hz / 1e6,
                     s_ppa_voltea && s_gamma == 1.0f ? "todo en el PPA + DMA desde PSRAM" : "CPU con curva gamma");
        }
    }
}

// BIOMA terminó un cuadro (núcleo 0, con el PPA libre): el PPA recorta el centro a 320x240 en un cuadro que
// nadie está usando, y ése pasa a ser "el nuevo"
void lcd_cuadro_listo(const uint16_t *img)
{
    if (!s_ok) return;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) != pdTRUE) return;
    int w = 0;
    while (w < NCQ && (w == s_ia || w == s_ib || w == s_uso[0] || w == s_uso[1] || w == s_uso[2])) w++;
    if (w == NCQ) { xSemaphoreGive(s_mux); return; }
    bool tierra = bioma_escena() == 1;                  // TIERRA VIVA se dibuja al tamaño de la pantalla: 1:1, sin escalar
    ppa_srm_oper_config_t op = {
        .in = { .buffer = img, .pic_w = TW, .pic_h = TH, .block_w = tierra ? ANCHO : REC_W, .block_h = tierra ? ALTO : REC_H,
                .block_offset_x = tierra ? TIERRA_LCD_X : REC_X, .block_offset_y = tierra ? TIERRA_LCD_Y : REC_Y,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = s_cq[w], .buffer_size = CUADRO, .pic_w = ANCHO, .pic_h = ALTO, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = tierra ? 1.0f : 15.0f / 16.0f, .scale_y = tierra ? 1.0f : 15.0f / 16.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa_rec, &op) == ESP_OK) {
        int64_t t = esp_timer_get_time();
        if (s_ib >= 0) {
            float dt = (float)(t - s_tb);
            if (dt > 300000.0f) dt = 300000.0f;
            if (dt < 30000.0f) dt = 30000.0f;
            s_periodo = s_periodo * 0.8f + 0.2f * dt;
            s_fps_bioma = 1e6f / s_periodo;
        }
        s_ia = s_ib < 0 ? w : s_ib;
        s_ib = w;
        s_tb = t;
        s_seq++;
    }
    xSemaphoreGive(s_mux);
    xTaskNotifyGive(s_task);
}

void lcd_estado_json(char *b, size_t cap)
{
    snprintf(b, cap, "{\"ok\":%s,\"fps\":%.1f,\"fps_bioma\":%.1f,\"cuadros\":%lu,\"toques\":%lu,\"ultimo_toque\":[%d,%d],\"brillo\":%d,\"gamma\":%.2f,\"spi_mhz\":%.1f,\"mezcla_ms\":%.2f,\"envio_ms\":%.1f,\"tactil_crudo\":{\"irq\":%d,\"x\":%d,\"y\":%d,\"z\":%d}}",
             s_ok ? "true" : "false", s_fps, s_fps_bioma, (unsigned long)s_cuadros, (unsigned long)s_toques, s_tx, s_ty, s_brillo,
             s_gamma, s_spi_hz / 1e6, s_t_mezcla, s_t_envio, s_diag_irq, s_diag_x, s_diag_y, s_diag_z);
}

static esp_err_t h_pantalla(httpd_req_t *r)
{
    char q[64], v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "brillo", v, sizeof(v)) == ESP_OK)
        brillo(atoi(v));
    if (httpd_query_key_value(q, "gamma", v, sizeof(v)) == ESP_OK) {
        float g = strtof(v, NULL);
        if (g >= 0.4f && g <= 1.6f) tablas_color(g);
    }
    if (httpd_query_key_value(q, "mhz", v, sizeof(v)) == ESP_OK) {   // se guarda y vale desde el próximo arranque
        int m = atoi(v);
        nvs_handle_t h;
        if (m >= 10 && m <= 80 && nvs_open("lcd", NVS_READWRITE, &h) == ESP_OK) { nvs_set_i32(h, "mhz", m); nvs_commit(h); nvs_close(h); }
    }
    char b[400];
    lcd_estado_json(b, sizeof(b));
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, b);
}

void lcd_registrar(httpd_handle_t s)
{
    httpd_uri_t u = { .uri = "/api/pantalla", .method = HTTP_GET, .handler = h_pantalla };
    httpd_register_uri_handler(s, &u);
}

void lcd_start(void)
{
    // 1) alimentación fabricada con dos pines, antes que cualquier señal
    pines_salida(P_GND, 0);
    gpio_reset_pin(P_VCC);
    gpio_set_direction(P_VCC, GPIO_MODE_INPUT);
    gpio_pulldown_en(P_VCC);
    vTaskDelay(pdMS_TO_TICKS(10));
    // Antes: si IO15 se leía en alto se suponía un cable de 3,3 V y el pin quedaba como entrada. Falló: encendido por
    // el USB HUSB, la pantalla se alimentaba "de rebote" por sus señales, IO15 se leía en alto y el táctil quedaba sin
    // corriente (0 toques). Ahora IO15 SIEMPRE entrega 3,3 V: con un cable de 3,3 V puesto es el mismo voltaje.
    if (gpio_get_level(P_VCC) == 1) ESP_LOGW(TAG, "IO15 ya estaba en alto al arrancar (alimentación de rebote o cable): igual se enciende");
    gpio_pulldown_dis(P_VCC);
    pines_salida(P_VCC, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    // 2) reset, táctil y luz de fondo
    pines_salida(P_RST, 1);
    pines_salida(T_CS, 1);
    pines_salida(T_CLK, 0);
    pines_salida(T_DIN, 0);
    gpio_reset_pin(T_DO);  gpio_set_direction(T_DO, GPIO_MODE_INPUT);
    gpio_reset_pin(T_IRQ); gpio_set_direction(T_IRQ, GPIO_MODE_INPUT); gpio_pullup_en(T_IRQ);
    ledc_timer_config_t lt = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .gpio_num = P_LED, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                 .timer_sel = LEDC_TIMER_0, .duty = 0 };
    ledc_channel_config(&lc);
    gpio_set_level(P_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(P_RST, 1); vTaskDelay(pdMS_TO_TICKS(150));
    // 3) bus SPI (por la matriz de pines: cualquier pin sirve)
    spi_bus_config_t bus = { .sclk_io_num = P_SCK, .mosi_io_num = P_MOSI, .miso_io_num = P_MISO,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = CUADRO / 5 };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "sin bus SPI"); return; }
    // velocidad: 40 MHz (lo que usa casi todo el mundo con esta pantalla); se puede cambiar por /api/pantalla?mhz=
    // y queda en la NVS. Dedo apoyado en la pantalla al arrancar = modo seguro a 20 MHz.
    int mhz = 40;
    nvs_handle_t h;
    if (nvs_open("lcd", NVS_READONLY, &h) == ESP_OK) { int32_t m; if (nvs_get_i32(h, "mhz", &m) == ESP_OK) mhz = m; nvs_close(h); }
    if (gpio_get_level(T_IRQ) == 0) { mhz = 20; ESP_LOGW(TAG, "dedo en la pantalla al arrancar: modo seguro a 20 MHz"); }
    static const int respaldo[] = { 0, 26, 20, 10 };
    for (int i = 0; i < 4; i++) {
        int m = i ? respaldo[i] : mhz;
        if (i && m >= mhz) continue;
        esp_lcd_panel_io_spi_config_t io = { .cs_gpio_num = P_CS, .dc_gpio_num = P_DC, .spi_mode = 0, .pclk_hz = m * 1000 * 1000,
                                             .trans_queue_depth = 8, .on_color_trans_done = on_listo,
                                             .lcd_cmd_bits = 8, .lcd_param_bits = 8, .flags.psram_dma_direct = 1 };
        if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io) == ESP_OK) { s_spi_hz = m * 1000 * 1000; break; }
        ESP_LOGW(TAG, "SPI a %d MHz no se pudo, bajando", m);
    }
    if (!s_io) { ESP_LOGE(TAG, "sin IO"); return; }
    for (int i = 0; i < 2; i++) s_buf[i] = heap_caps_malloc(ANCHO * FRANJA * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_libres = xSemaphoreCreateCounting(2, 2);
    if (!s_buf[0] || !s_buf[1] || !s_libres) { ESP_LOGE(TAG, "falta memoria"); return; }
    // 4) ILI9341: la secuencia completa de arranque (fuentes, VCOM, gamma). Con la mínima quedaba gris.
    cmd(0x01, NULL, 0); vTaskDelay(pdMS_TO_TICKS(150));
    static const uint8_t ini[] = {
        0xEF, 3, 0x03, 0x80, 0x02,   0xCF, 3, 0x00, 0xC1, 0x30,   0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,   0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,   0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,   0xC0, 1, 0x23,   0xC1, 1, 0x10,   0xC5, 2, 0x3E, 0x28,   0xC7, 1, 0x86,
        0x36, 1, 0x28,   0x37, 1, 0x00,   0x3A, 1, 0x55,   0xB1, 2, 0x00, 0x18,   0xB6, 3, 0x08, 0x82, 0x27,
        0xF2, 1, 0x00,   0x26, 1, 0x01,
        0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
        0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
        0x00 };
    for (int i = 0; ini[i]; ) { cmd(ini[i], &ini[i + 2], ini[i + 1]); i += 2 + ini[i + 1]; }
    cmd(0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x29, NULL, 0); vTaskDelay(pdMS_TO_TICKS(150));
    // prueba de colores al arrancar: rojo, verde, azul (así se ve de inmediato si el color y el orden están bien)
    static const uint16_t colores[3] = { 0xF800, 0x07E0, 0x001F };
    for (int c = 0; c < 3; c++) {
        uint16_t px = (uint16_t)((colores[c] << 8) | (colores[c] >> 8));
        for (int i = 0; i < ANCHO * FRANJA; i++) s_buf[0][i] = px;
        for (int y0 = 0; y0 < ALTO; y0 += FRANJA) {
            xSemaphoreTake(s_libres, portMAX_DELAY);
            ventana(y0, y0 + FRANJA - 1);
            esp_lcd_panel_io_tx_color(s_io, 0x2C, s_buf[0], ANCHO * FRANJA * 2);
        }
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    // 5) el PPA: recorte (SRM) y mezcla (blend), con sus cuadros en PSRAM
    for (int i = 0; i < NCQ; i++) s_cq[i] = heap_caps_aligned_calloc(64, 1, CUADRO, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    for (int i = 0; i < 2; i++) s_mix[i] = heap_caps_aligned_calloc(64, 1, CUADRO, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_mux = xSemaphoreCreateMutex();
    s_mez_lista = xSemaphoreCreateCounting(4, 0);
    for (int i = 0; i < 2; i++) s_spi[i] = heap_caps_aligned_calloc(64, 1, CUADRO, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ppa_client_config_t pr = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ppa_client_config_t pm = { .oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 2 };
    if (!s_cq[NCQ - 1] || !s_mix[1] || !s_spi[1] || !s_mux || !s_mez_lista ||
        ppa_register_client(&pr, &s_ppa_rec) != ESP_OK || ppa_register_client(&pm, &s_ppa_mez) != ESP_OK) {
        ESP_LOGE(TAG, "falta memoria o PPA para la pantalla");
        return;
    }
    ppa_event_callbacks_t cb = { .on_trans_done = on_mezcla };
    ppa_client_register_event_callbacks(s_ppa_mez, &cb);
    for (int i = 0; i < NCQ; i++) esp_cache_msync(s_cq[i], CUADRO, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    for (int i = 0; i < 2; i++) {
        esp_cache_msync(s_mix[i], CUADRO, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync(s_spi[i], CUADRO, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    // autoprueba: ¿el volteo del PPA da EXACTAMENTE lo mismo que voltear con la CPU? (todos los colores posibles)
    {
        uint16_t *t = s_cq[0];
        for (int i = 0; i < ANCHO * ALTO; i++) t[i] = (uint16_t)(i * 40503u);   // recorre los 65.536 colores
        esp_cache_msync(t, CUADRO, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        int malos = -1;
        if (mezcla(t, t, 0, true, s_spi[0], PPA_TRANS_MODE_BLOCKING) == ESP_OK) {
            xSemaphoreTake(s_mez_lista, pdMS_TO_TICKS(10));  // (el aviso de fin también llega aquí)
            malos = 0;
            for (int i = 0; i < ANCHO * ALTO; i++) if (s_spi[0][i] != bswap(t[i])) malos++;
        }
        s_ppa_voltea = malos == 0;
        memset(t, 0, CUADRO);
        esp_cache_msync(t, CUADRO, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (s_ppa_voltea) ESP_LOGI(TAG, "autoprueba: el PPA voltea los bytes idéntico a la CPU (76.800 píxeles): la CPU queda libre");
        else ESP_LOGW(TAG, "autoprueba: el volteo del PPA no calza (%d píxeles distintos): se voltea con la CPU", malos);
    }
    tablas_color(s_gamma);
    gpio_set_drive_capability(P_LED, GPIO_DRIVE_CAP_3);   // la luz de fondo pide corriente: fuerza máxima
    brillo(100);
    cal_cargar();
    s_calibrar = !s_cal_ok;                              // sin calibración guardada: calibrar al arrancar
    s_ok = true;
    xTaskCreatePinnedToCore(lcd_task, "pantalla", 4096, NULL, 9, &s_task, 1);
    ESP_LOGI(TAG, "pantalla lista: ILI9341 320x240 a %.1f MHz, táctil XPT2046, PPA recorte+mezcla", s_spi_hz / 1e6);
}
