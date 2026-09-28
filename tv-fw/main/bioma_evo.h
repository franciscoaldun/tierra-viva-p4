// BIOMA — evolución juzgada por el tamaño del JPEG (C puro: la misma lógica corre en el P4 y en el PC).
//
// (1+1)-ES con ventanas PAREADAS: primero se mide el genoma actual ("incumbente") durante una ventana,
// después el candidato mutado durante otra, y los dos se comparan contra el MISMO objetivo.
// De cada ventana sólo se mide la segunda mitad: el mundo es persistente y la primera mitad es la transición
// desde lo que dejó el genoma anterior (host_test mostró que la red tarda cientos de cuadros en asentarse).
//
// El objetivo de complejidad sigue el reloj del núcleo LP (±15 %): más rico al atardecer, más simple al amanecer.
#pragma once
#include <stdint.h>
#include "bioma_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { EVO_JPEG = 0, EVO_RANDOM = 1, EVO_FROZEN = 2 } evo_mode_t;   // juez real y 2 controles

typedef struct {
    genome_t inc, trial;
    evo_mode_t mode;
    int      window;          // cuadros por ventana
    int      warmup;          // ventanas iniciales sin evolución (la red nace del ruido)
    int      phase;           // 0 = midiendo el incumbente, 1 = midiendo el candidato
    int      n;               // cuadros transcurridos en la ventana actual
    double   acc_kb;          // suma (ponderada por cuadros) de la segunda mitad
    int      acc_n;
    float    kb_inc;          // medición de la última ventana del incumbente
    float    base_kb;         // complejidad de referencia (fin del calentamiento)
    float    amp;             // amplitud del ritmo diario del objetivo (0,15 = ±15 %)
    float    last_target, last_kb_inc, last_kb_trial;
    uint32_t generation, accepted, windows;
    uint32_t rng;
} evo_t;

void  evo_init(evo_t *e, const genome_t *g0, int window, uint32_t seed, evo_mode_t mode);
const genome_t *evo_current(const evo_t *e);             // el genoma que hay que simular ahora
float evo_target(const evo_t *e, float day01);           // objetivo en KB para esa hora (0 = medianoche)
// Se llama por cada cuadro codificado (en el PC se puede pasar un tamaño representativo de n cuadros).
// Devuelve 1 cuando terminó una generación (se decidió si el candidato reemplaza al incumbente).
int   evo_feed(evo_t *e, float jpeg_kb, float day01, int n);

#ifdef __cplusplus
}
#endif
