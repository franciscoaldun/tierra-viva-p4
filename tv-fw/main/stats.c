// Medidor de uso (versión recortada de Cerebro P4): % del tiempo que cada acelerador pasa ocupado,
// % de CPU por núcleo, temperatura del chip y cuadros por segundo. Ventanas de 1 segundo.
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "app.h"

static const char *TAG = "stats";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_busy_acc[ENG_COUNT];
static int64_t s_busy_last_end[ENG_COUNT];
static float   s_eng_pct[ENG_COUNT];
static uint32_t s_frames;
static uint64_t s_net_rx, s_net_tx;
static float s_fps, s_cpu[2], s_temp = -1;
static temperature_sensor_handle_t s_tsens;

void stats_busy(eng_t e, int64_t t0, int64_t t1)
{
    portENTER_CRITICAL(&s_mux);
    if (t0 < s_busy_last_end[e]) t0 = s_busy_last_end[e];      // unión de intervalos: no contar doble
    if (t1 > t0) {
        s_busy_acc[e] += t1 - t0;
        s_busy_last_end[e] = t1;
    }
    portEXIT_CRITICAL(&s_mux);
}

void stats_net(size_t rx, size_t tx) { portENTER_CRITICAL(&s_mux); s_net_rx += rx; s_net_tx += tx; portEXIT_CRITICAL(&s_mux); }
void stats_frame(void)               { portENTER_CRITICAL(&s_mux); s_frames++; portEXIT_CRITICAL(&s_mux); }
float stats_eng_pct(eng_t e)         { return s_eng_pct[e]; }
float stats_cpu_pct(int core)        { return s_cpu[core & 1]; }
float stats_fps(void)                { return s_fps; }
float stats_temp(void)               { return s_temp; }

static void stats_task(void *arg)
{
    int64_t t_prev = esp_timer_get_time();
    uint64_t idle_prev[2];
    uint32_t f0 = 0;
    for (int c = 0; c < 2; c++) idle_prev[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time(), dt = now - t_prev;
        t_prev = now;
        portENTER_CRITICAL(&s_mux);
        for (int e = 0; e < ENG_COUNT; e++) {
            float p = 100.0f * (float)s_busy_acc[e] / (float)dt;
            s_eng_pct[e] = p > 100 ? 100 : p;
            s_busy_acc[e] = 0;
        }
        uint32_t f = s_frames;
        portEXIT_CRITICAL(&s_mux);
        s_fps = (f - f0) / (dt / 1e6f);
        f0 = f;
        for (int c = 0; c < 2; c++) {          // CPU = 100% - tiempo de la tarea IDLE
            uint64_t rt = ulTaskGetIdleRunTimeCounterForCore(c);
            float idle = 100.0f * (float)(rt - idle_prev[c]) / (float)dt;
            idle_prev[c] = rt;
            s_cpu[c] = 100.0f - (idle > 100 ? 100 : idle);
        }
        if (s_tsens) {
            float t;
            if (temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) s_temp = t;
        }
        static int k;
        if (++k % 5 == 0) {
            ESP_LOGI(TAG, "%.1f fps | CPU %.0f/%.0f%% | JPEG %.0f%% PPA %.0f%% USB %.0f%% | %.1f C | día %.2f",
                     s_fps, s_cpu[0], s_cpu[1], s_eng_pct[ENG_JENC], s_eng_pct[ENG_PPA], s_eng_pct[ENG_USB], s_temp,
                     lpcore_day_phase() / 65536.0f);
        }
    }
}

void stats_start(void)
{
    // el sensor del P4 exige UN rango: 20..100 °C funciona (lo mismo que en horno-fractal)
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
        ESP_LOGW(TAG, "sin sensor de temperatura");
    }
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, 15, NULL, 0);
}
