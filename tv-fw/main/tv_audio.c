// tv_audio.c — el sonido de BIOMA para la tele: un pin (IO22) con PDM a 6,1 MHz → filtro RC → RCA blanco.
// El I2S0 del P4 convierte PCM a PDM por hardware (modo "DAC de una línea"); el reloj sale del cristal
// (el APLL es del video y no se toca).
//
// Qué suena: un ambiente generativo suave. Cuatro voces tocan notas de una escala pentatónica que camina al azar,
// con subidas y bajadas lentas (segundos). La noche de BIOMA suena grave y lenta; el día, más arriba y más seguido.
// Cuando la evolución acepta un genoma nuevo suena una campanita. Todo con tablas: nada de sinf por muestra.
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_random.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tv.h"
#include "app.h"

static const char *TAG = "sonido";

#define FS        48000
#define BLOQUE    480                       // 10 ms por escritura
#define N_VOCES   4
#define TABLA     2048

static i2s_chan_handle_t s_tx;
static int16_t s_seno[TABLA];
static volatile bool s_on;
static volatile int s_vol = 35;
static volatile bool s_campana;
static volatile float s_dia = 0.5f;

typedef struct {
    uint32_t fase, paso;                    // oscilador (fase de 32 bits → tabla de 2048)
    uint32_t fase2, paso2;                  // una octava arriba, más suave (timbre de órgano)
    int32_t  env, env_obj, env_vel;         // envolvente en 1/65536
    int      vida;                          // bloques que le quedan antes de soltar la nota
} voz_t;

static voz_t s_v[N_VOCES];
static uint32_t s_rnd = 12345;
static uint32_t rnd(void) { uint32_t x = s_rnd; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s_rnd = x; }

// campanita: tres parciales inarmónicos que se apagan solos
static uint32_t s_cf[3], s_cp[3];
static int32_t  s_ca;

static uint32_t paso_hz(float hz) { return (uint32_t)(hz * 4294967296.0 / FS); }

static void nueva_nota(voz_t *v)
{
    // pentatónica menor de re: re fa sol la do, en dos octavas; de noche una octava más abajo
    static const float semis[] = { 0, 3, 5, 7, 10, 12, 15, 17, 19, 22 };
    static int grado = 4;
    grado += (int)(rnd() % 5) - 2;
    if (grado < 0) grado = 1;
    if (grado > 9) grado = 8;
    float base = 146.83f * (s_dia < 0.3f || s_dia > 0.8f ? 0.5f : 1.0f);    // re3 (de noche re2)
    float hz = base * powf(2.0f, semis[grado] / 12.0f);
    v->paso = paso_hz(hz);
    v->paso2 = paso_hz(hz * 2.0f * 1.003f);                                // un poquito desafinada: más viva
    v->env_obj = 65536;
    v->env_vel = 65536 / (FS * 3 / BLOQUE);                                // sube en ~3 s
    v->vida = (int)(400 + rnd() % 500);                                     // 4-9 s sonando
}

static void campana_empezar(void)
{
    static const float f[3] = { 1318.5f, 3639.0f, 7119.0f };               // mi6 y parciales de campana
    for (int i = 0; i < 3; i++) { s_cf[i] = paso_hz(f[i]); s_cp[i] = 0; }
    s_ca = 65536;
}

static void audio_task(void *arg)
{
    static int16_t buf[BLOQUE];
    int bloques = 0;
    while (1) {
        if (!s_on) {
            memset(buf, 0, sizeof(buf));
        } else {
            // ¿alguna voz libre? de día entran notas más seguido
            int cada = (s_dia > 0.3f && s_dia < 0.8f) ? 180 : 320;
            if (++bloques >= cada) {
                bloques = (int)(rnd() % 60);
                for (int k = 0; k < N_VOCES; k++) if (s_v[k].env == 0 && s_v[k].env_obj == 0) { nueva_nota(&s_v[k]); break; }
            }
            if (s_campana) { s_campana = false; campana_empezar(); }
            for (int k = 0; k < N_VOCES; k++) {                            // envolventes (una vez por bloque)
                voz_t *v = &s_v[k];
                if (v->env_obj && --v->vida <= 0) { v->env_obj = 0; v->env_vel = 65536 / (FS * 5 / BLOQUE); }  // se apaga en ~5 s
                if (v->env < v->env_obj) { v->env += v->env_vel; if (v->env > v->env_obj) v->env = v->env_obj; }
                else if (v->env > v->env_obj) { v->env -= v->env_vel; if (v->env < 0) v->env = 0; }
            }
            int vol = s_vol;
            for (int i = 0; i < BLOQUE; i++) {
                int32_t acc = 0;
                for (int k = 0; k < N_VOCES; k++) {
                    voz_t *v = &s_v[k];
                    if (!v->env) continue;
                    v->fase += v->paso;
                    v->fase2 += v->paso2;
                    int32_t s = s_seno[v->fase >> 21] + (s_seno[v->fase2 >> 21] >> 2);
                    acc += (s * (v->env >> 4)) >> 12;
                }
                if (s_ca > 0) {
                    int32_t c = 0;
                    for (int j = 0; j < 3; j++) { s_cp[j] += s_cf[j]; c += s_seno[s_cp[j] >> 21] >> (j + 1); }
                    acc += (c * (s_ca >> 4)) >> 12;
                    s_ca -= s_ca >> 13;                                     // se apaga en ~1,5 s
                    if (s_ca < 64) s_ca = 0;
                }
                acc = acc * vol / 100 / 3;
                buf[i] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
            }
        }
        size_t escrito = 0;
        i2s_channel_write(s_tx, buf, sizeof(buf), &escrito, portMAX_DELAY);
    }
}

void tv_audio_start(void)
{
    for (int i = 0; i < TABLA; i++) s_seno[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * (float)M_PI * i / TABLA));
    s_rnd = esp_random() | 1;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = BLOQUE;
    cc.auto_clear = true;                   // si la tarea se atrasa, sale silencio y no un zumbido
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) { ESP_LOGE(TAG, "sin canal I2S"); return; }
    i2s_pdm_tx_config_t pc = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(FS),
        .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .clk = GPIO_NUM_NC, .dout = TV_PIN_AUDIO, .dout2 = GPIO_NUM_NC },
    };
    pc.clk_cfg.clk_src = I2S_CLK_SRC_XTAL;  // el APLL es del video
    if (i2s_channel_init_pdm_tx_mode(s_tx, &pc) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGE(TAG, "el PDM no arrancó");
        return;
    }
    // prioridad sobre la simulación (8): escribe 10 ms de sonido y duerme; si no le toca turno se corta
    xTaskCreatePinnedToCore(audio_task, "sonido", 4096, NULL, 11, NULL, 1);
    ESP_LOGI(TAG, "sonido PDM en IO%d (%d Hz)", TV_PIN_AUDIO, FS);
}

void tv_audio_set(bool on, int volumen) { s_on = on; s_vol = volumen < 0 ? 0 : volumen > 100 ? 100 : volumen; }
void tv_audio_campana(void) { s_campana = true; }
void tv_audio_dia(float fase) { s_dia = fase; }
