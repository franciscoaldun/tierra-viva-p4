// Arranca el núcleo LP del P4 con su programa (ulp/main.c): el "reloj de reposo" que lleva la hora del día.
// Usa el temporizador RTC, el mismo que sigue contando aunque los núcleos grandes duerman.
#include "esp_log.h"
#include "ulp_lp_core.h"
#include "lp_core_main.h"
#include "app.h"

extern const uint8_t lp_core_main_bin_start[] asm("_binary_lp_core_main_bin_start");
extern const uint8_t lp_core_main_bin_end[]   asm("_binary_lp_core_main_bin_end");

static bool s_ok;

void lpcore_start(void)
{
    ulp_lp_core_cfg_t cfg = { .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU };
    if (ulp_lp_core_load_binary(lp_core_main_bin_start, lp_core_main_bin_end - lp_core_main_bin_start) == ESP_OK &&
        ulp_lp_core_run(&cfg) == ESP_OK) {
        s_ok = true;
        ESP_LOGI("lp", "núcleo LP corriendo (reloj del día)");
    } else {
        ESP_LOGE("lp", "no se pudo arrancar el núcleo LP");
    }
}

uint32_t lpcore_day_phase(void) { return s_ok ? ulp_lp_day_phase : 32768; }   // sin LP: mediodía fijo
uint32_t lpcore_speed(void)     { return s_ok ? ulp_lp_speed : 0; }

void lpcore_set_time(uint32_t sec_of_day)
{
    if (!s_ok) return;
    ulp_lp_sync_s = sec_of_day % 86400;
    ulp_lp_sync_req = 1;
}

void lpcore_set_speed(uint32_t speed)
{
    if (!s_ok) return;
    ulp_lp_speed = speed < 1 ? 1 : speed > 8640 ? 8640 : speed;
}
