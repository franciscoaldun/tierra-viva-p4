// Programa del núcleo LP (RISC-V de bajo consumo): el "reloj de reposo" de BIOMA.
// Cuenta el tiempo con el temporizador RTC (ticks del reloj lento, calibrados por el núcleo grande al arrancar)
// y publica la fase del día (0 = medianoche, 32768 = mediodía). Los núcleos grandes sólo la leen.
#include <stdint.h>
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_lp_timer_shared.h"

volatile uint32_t lp_day_phase = 32768;   // 0..65535
volatile uint32_t lp_speed = 144;         // 1 = día real · 144 = un día cada 10 minutos
volatile uint32_t lp_sync_s = 0;          // hora real (segundos desde medianoche) que manda el PC
volatile uint32_t lp_sync_req = 0;        // el núcleo grande lo pone en 1 para sincronizar
volatile uint32_t lp_ticks_s = 0;         // ticks del reloj lento por segundo (para verificar)
volatile uint32_t lp_loops = 0;

#define DAY_MS 86400000ULL

int main(void)
{
    uint64_t tps = ulp_lp_core_lp_timer_calculate_sleep_ticks(1000000);
    if (tps == 0) tps = 136000;           // por si la calibración aún no está
    lp_ticks_s = (uint32_t)tps;
    uint64_t t0 = ulp_lp_core_lp_timer_get_cycle_count();
    uint64_t base_ms = DAY_MS / 2;        // arranca a mediodía hasta que el PC diga la hora
    uint32_t speed = lp_speed;
    while (1) {
        uint64_t now = ulp_lp_core_lp_timer_get_cycle_count();
        uint64_t day_ms = (base_ms + (now - t0) * 1000 / tps * speed) % DAY_MS;
        if (lp_sync_req) {                // hora real desde el PC
            base_ms = (uint64_t)lp_sync_s * 1000;
            t0 = now;
            lp_sync_req = 0;
            day_ms = base_ms;
        } else if (lp_speed != speed) {   // cambio de velocidad sin salto: se rebasa en la hora actual
            base_ms = day_ms;
            t0 = now;
            speed = lp_speed;
        }
        lp_day_phase = (uint32_t)(day_ms * 65536 / DAY_MS);
        lp_loops++;
        ulp_lp_core_delay_us(50000);      // 20 lecturas por segundo sobran para un sol
    }
    return 0;
}
