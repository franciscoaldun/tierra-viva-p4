// BIOMA — evolución juzgada por el JPEG (ver bioma_evo.h). C puro, sin nada del ESP-IDF.
#include <math.h>
#include "bioma_evo.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint32_t xs(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static float frand(uint32_t *s) { return (xs(s) & 0xFFFFFF) / 16777216.0f; }
static float gauss(uint32_t *s) { float a = 0; for (int i = 0; i < 6; i++) a += frand(s); return (a - 3.0f) * 1.414f; }  // σ≈1
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static genome_t mutate(genome_t g, uint32_t *s)
{
    g.sa = clampf(g.sa * (1 + 0.10f * gauss(s)), 0.15f, 1.2f);
    g.ra = clampf(g.ra * (1 + 0.10f * gauss(s)), 0.15f, 1.0f);
    g.sd = clampf(g.sd * (1 + 0.10f * gauss(s)), 4.0f, 24.0f);
    g.ss = clampf(g.ss * (1 + 0.10f * gauss(s)), 0.6f, 2.0f);
    g.decay = clampf(g.decay + 0.015f * gauss(s), 0.80f, 0.97f);
    g.dep = clampf(g.dep * (1 + 0.10f * gauss(s)), 0.5f, 6.0f);
    g.respawn = clampf(g.respawn * (1 + 0.20f * gauss(s)), 0.001f, 0.03f);
    return g;
}

void evo_init(evo_t *e, const genome_t *g0, int window, uint32_t seed, evo_mode_t mode)
{
    *e = (evo_t){ 0 };
    e->inc = e->trial = *g0;
    e->mode = mode;
    e->window = window < 20 ? 20 : window;
    e->warmup = 2;
    e->amp = 0.15f;                             // ±15 %: el rango que el juez alcanzó a mover en host_test (exp. 2)
    e->rng = seed ? seed : 0x9E3779B9u;
}

const genome_t *evo_current(const evo_t *e)
{
    return e->phase == 1 ? &e->trial : &e->inc;
}

// más rico a las 18:00, más simple a las 06:00
float evo_target(const evo_t *e, float day01)
{
    return e->base_kb * (1.0f + e->amp * sinf(2 * (float)M_PI * (day01 - 0.5f)));
}

int evo_feed(evo_t *e, float kb, float day01, int n)
{
    if (n < 1) n = 1;
    int before = e->n;
    e->n += n;
    int half = e->window / 2;
    if (e->n > half) {                          // sólo cuenta la segunda mitad de la ventana
        int counted = e->n - (before > half ? before : half);
        e->acc_kb += (double)kb * counted;
        e->acc_n += counted;
    }
    if (e->n < e->window) return 0;

    float mean = e->acc_n ? (float)(e->acc_kb / e->acc_n) : kb;
    e->n = 0; e->acc_kb = 0; e->acc_n = 0;
    e->windows++;

    if (e->windows <= (uint32_t)e->warmup) {    // calentamiento: la red se forma, sin evolucionar
        e->base_kb = mean;
        e->kb_inc = mean;
        e->last_kb_inc = mean;
        if (e->windows == (uint32_t)e->warmup && e->mode != EVO_FROZEN) {
            e->trial = mutate(e->inc, &e->rng);
            e->phase = 1;                       // la próxima ventana mide al primer candidato
        }
        return 0;
    }
    if (e->mode == EVO_FROZEN) {                // control: el mismo genoma para siempre
        e->kb_inc = e->last_kb_inc = mean;
        e->last_target = evo_target(e, day01);
        e->generation++;
        return 1;
    }
    if (e->phase == 0) {                        // terminó la ventana del incumbente → ahora el candidato
        e->kb_inc = e->last_kb_inc = mean;
        e->trial = mutate(e->inc, &e->rng);
        e->phase = 1;
        return 0;
    }
    // terminó la ventana del candidato: se decide contra el objetivo de ESTA hora
    float target = evo_target(e, day01);
    e->last_target = target;
    e->last_kb_trial = mean;
    int accept;
    if (e->mode == EVO_RANDOM) accept = (xs(&e->rng) & 1);                  // control: moneda al aire
    else                       accept = fabsf(mean - target) < fabsf(e->kb_inc - target);
    if (accept) { e->inc = e->trial; e->accepted++; }
    e->generation++;
    e->phase = 0;                               // se vuelve a medir el incumbente (el mundo cambió)
    return 1;
}
