// BIOMA — núcleo de la simulación, SIN nada del ESP-IDF: el mismo código corre en el P4 y en el PC
// (host_test/ lo compila con gcc para verificarlo antes de tocar el chip).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TW 480                // mapa de rastro (se escala ×4 a 1920x1080)
#define TH 270
#define LUT_N 1024            // tabla de seno/coseno

typedef struct {
    float sa;       // ángulo de los sensores (rad)
    float ra;       // giro por paso (rad)
    float sd;       // distancia de los sensores (celdas)
    float ss;       // largo del paso (celdas)
    float decay;    // evaporación por paso (0..1)
    float dep;      // cuánto rastro deja cada agente
    float respawn;  // fracción de agentes que renace al azar en cada paso (evita que la red colapse en pocas hebras)
    int   palette;  // familia de colores (reservado)
} genome_t;

// 6 bytes (antes 8 con relleno): en el P4 los agentes viven en la PSRAM y cada paso los lee y escribe enteros,
// así que 25 % menos bytes = 25 % menos tráfico (medido: el paso de agentes era el 77 % del cuadro)
typedef struct { uint16_t x, y, a; } agent_t;        // x,y en punto fijo 9.7 (celdas × 128); a: 0..65535 = 0..2π

extern uint16_t *sim_row[TH];     // filas del rastro: pueden vivir en trozos distintos de memoria

void sim_init_tables(void);
void sim_seed_agents(agent_t *ag, int n, uint32_t (*rnd32)(void));
void sim_agents_step(agent_t *ag, int first, int last, const genome_t *g, uint32_t seed);
void sim_snapshot_edges(void);                                    // antes de cada difusión
// ordena los agentes por fila (counting sort, O(n)) de src a dst: los agentes seguidos quedan cerca en el mapa,
// así sus lecturas y depósitos caen en la caché (medido en el P4: el paso de agentes era el 77 % del cuadro)
void sim_sort_agents(const agent_t *src, agent_t *dst, int n);
void sim_diffuse_half(int half, float decay, uint32_t hist[64]);  // half 0 = filas [0,135), 1 = [135,270)
void sim_build_palette(float day);                                // 0 = noche, 1 = día
uint16_t sim_palette(int i);                                      // color i (0..255) de la paleta actual
void sim_colorize(uint16_t *out565, const uint32_t hist0[64], const uint32_t hist1[64]);
// lo mismo para las filas [y0, y1) (para repartir el coloreado entre los 2 núcleos); out565 = imagen completa
void sim_colorize_rows(uint16_t *out565, const uint32_t hist0[64], const uint32_t hist1[64], int y0, int y1);
// la misma imagen en escala de grises (sin paleta): es lo que mira el juez, para que los colores del día y
// la noche no cambien el tamaño del JPEG (host_test mostró que la paleta contamina la medición)
void sim_gray(uint8_t *out8, const uint32_t hist0[64], const uint32_t hist1[64]);

#ifdef __cplusplus
}
#endif
