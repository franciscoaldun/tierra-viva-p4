// BIOMA — núcleo de la simulación Physarum (C puro: corre igual en el P4 y en el PC).
#include <string.h>
#include <math.h>
#include "bioma_sim.h"

// en el P4 los bucles calientes van a la RAM de instrucciones (sin esperas de la flash); en el PC no hace nada
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SIM_HOT IRAM_ATTR
#else
#define SIM_HOT
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

uint16_t *sim_row[TH];
static int16_t  s_sin[LUT_N], s_cos[LUT_N];      // Q14
static uint16_t s_pal[256];                       // paleta RGB565
static uint8_t  s_gam[1024];                      // curva gamma 0,6
static uint16_t s_edge[4][TW];                    // filas de borde copiadas antes de difundir en el mismo arreglo

void sim_init_tables(void)
{
    for (int i = 0; i < LUT_N; i++) {
        s_sin[i] = (int16_t)lrintf(16384.0f * sinf(2 * (float)M_PI * i / LUT_N));
        s_cos[i] = (int16_t)lrintf(16384.0f * cosf(2 * (float)M_PI * i / LUT_N));
    }
    for (int i = 0; i < 1024; i++) s_gam[i] = (uint8_t)(255.0f * powf(i / 1023.0f, 0.6f));
    sim_build_palette(0.5f);
}

// agentes repartidos al azar: la red emerge del ruido
void sim_seed_agents(agent_t *ag, int n, uint32_t (*rnd32)(void))
{
    // multiplicar y desplazar (no "% N"): con módulo, las primeras 32 columnas y 242 filas salían con el doble
    // de agentes y dibujaban líneas fantasma (lo detectó host_test/ en el PC)
    for (int i = 0; i < n; i++) {
        ag[i].x = (uint16_t)(((uint64_t)rnd32() * (TW << 7)) >> 32);
        ag[i].y = (uint16_t)(((uint64_t)rnd32() * (TH << 7)) >> 32);
        ag[i].a = (uint16_t)(rnd32() >> 16);
    }
}

static inline uint16_t sample(int32_t fx, int32_t fy)
{
    int x = fx >> 7, y = fy >> 7;          // el mundo es un toro: se envuelve por los bordes
    if (x < 0) x += TW; else if (x >= TW) x -= TW;
    if (y < 0) y += TH; else if (y >= TH) y -= TH;
    return sim_row[y][x];
}

SIM_HOT void sim_agents_step(agent_t *ags, int first, int last, const genome_t *gp, uint32_t seed)
{
    const genome_t g = *gp;
    const int32_t sa = (int32_t)(g.sa * (65536.0f / (2 * (float)M_PI)));
    const int32_t ra = (int32_t)(g.ra * (65536.0f / (2 * (float)M_PI)));
    const int32_t sd = (int32_t)(g.sd * 128.0f);
    const int32_t ss = (int32_t)(g.ss * 128.0f);
    const uint32_t dep = (uint32_t)(g.dep * 64.0f);
    const int32_t WX = TW << 7, HY = TH << 7;
    const uint32_t reborn = (uint32_t)(g.respawn * 65536.0f);     // umbral sobre 16 bits de azar
    uint32_t rnd = seed | 1;
    for (int i = first; i < last; i++) {
        agent_t ag = ags[i];
        int32_t x = ag.x, y = ag.y, a = ag.a;
        rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
        if ((rnd >> 16) < reborn) {        // renace en un lugar y rumbo al azar: brotes nuevos que mantienen viva la red
            uint32_t r2 = rnd * 2654435761u;
            x = (int32_t)(((uint64_t)r2 * WX) >> 32);
            r2 ^= r2 << 13; r2 ^= r2 >> 17; r2 ^= r2 << 5;
            y = (int32_t)(((uint64_t)r2 * HY) >> 32);
            a = (int32_t)(rnd & 0xFFFF);
        }
        int iL = ((a - sa) >> 6) & (LUT_N - 1), iF = (a >> 6) & (LUT_N - 1), iR = ((a + sa) >> 6) & (LUT_N - 1);
        uint16_t L = sample(x + ((sd * s_cos[iL]) >> 14), y + ((sd * s_sin[iL]) >> 14));
        uint16_t F = sample(x + ((sd * s_cos[iF]) >> 14), y + ((sd * s_sin[iF]) >> 14));
        uint16_t R = sample(x + ((sd * s_cos[iR]) >> 14), y + ((sd * s_sin[iR]) >> 14));
        rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
        if (F >= L && F >= R) {
            // sigue derecho
        } else if (F < L && F < R) {
            a += (rnd & 1) ? ra : -ra;      // encrucijada: al azar
        } else if (L > R) {
            a -= ra;
        } else {
            a += ra;
        }
        a &= 0xFFFF;
        int k = (a >> 6) & (LUT_N - 1);
        x += (ss * s_cos[k]) >> 14;
        y += (ss * s_sin[k]) >> 14;
        if (x < 0) x += WX; else if (x >= WX) x -= WX;
        if (y < 0) y += HY; else if (y >= HY) y -= HY;
        ag.x = (uint16_t)x; ag.y = (uint16_t)y; ag.a = (uint16_t)a;
        ags[i] = ag;
        uint16_t *c = &sim_row[y >> 7][x >> 7];
        uint32_t v = *c + dep;             // (dos núcleos pueden pisarse un depósito: da igual, es rastro)
        *c = (uint16_t)(v > 65535 ? 65535 : v);
    }
}

void sim_sort_agents(const agent_t *src, agent_t *dst, int n)
{
    static uint32_t start[TH + 1];
    memset(start, 0, sizeof(start));
    for (int i = 0; i < n; i++) start[(src[i].y >> 7) + 1]++;
    for (int r = 0; r < TH; r++) start[r + 1] += start[r];
    for (int i = 0; i < n; i++) dst[start[src[i].y >> 7]++] = src[i];
}

void sim_snapshot_edges(void)
{
    memcpy(s_edge[0], sim_row[0], sizeof(s_edge[0]));
    memcpy(s_edge[1], sim_row[TH / 2 - 1], sizeof(s_edge[1]));
    memcpy(s_edge[2], sim_row[TH / 2], sizeof(s_edge[2]));
    memcpy(s_edge[3], sim_row[TH - 1], sizeof(s_edge[3]));
}

// difusión 3x3 + evaporación en el mismo arreglo, fila por fila con 2 filas de respaldo.
// prev0 = fila anterior a la primera (copia), last_next = fila siguiente a la última (copia).
SIM_HOT static void diffuse_rows(int r0, int r1, const uint16_t *prev0, const uint16_t *last_next, uint32_t k, uint32_t *hist)
{
    uint16_t prev[TW], cur[TW];
    memset(hist, 0, 64 * sizeof(uint32_t));
    memcpy(prev, prev0, sizeof(prev));
    for (int y = r0; y < r1; y++) {
        uint16_t *row = sim_row[y];
        memcpy(cur, row, sizeof(cur));
        const uint16_t *next = (y == r1 - 1) ? last_next : sim_row[y + 1];
        for (int x = 0; x < TW; x++) {
            int xm = x ? x - 1 : TW - 1, xp = (x == TW - 1) ? 0 : x + 1;
            uint32_t s = prev[xm] + prev[x] + prev[xp] + cur[xm] + cur[x] + cur[xp] + next[xm] + next[x] + next[xp];
            uint32_t v = (s * k) >> 16;
            row[x] = (uint16_t)v;
            hist[v >> 10]++;
        }
        memcpy(prev, cur, sizeof(prev));
    }
}

void sim_diffuse_half(int half, float decay, uint32_t hist[64])
{
    uint32_t k = (uint32_t)(decay / 9.0f * 65536.0f);
    if (half == 0) diffuse_rows(0, TH / 2, s_edge[3], s_edge[2], k, hist);       // arriba la 269, abajo la 135
    else           diffuse_rows(TH / 2, TH, s_edge[1], s_edge[0], k, hist);      // arriba la 134, abajo la 0
}

// ---------------- paleta día/noche ----------------
static uint16_t rgb565(float r, float g, float b)
{
    int R = r < 0 ? 0 : r > 255 ? 255 : (int)r, G = g < 0 ? 0 : g > 255 ? 255 : (int)g, B = b < 0 ? 0 : b > 255 ? 255 : (int)b;
    return (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
}

static void ramp(const float (*st)[4], int n, float t, float out[3])
{
    for (int k = 0; k < n - 1; k++) {
        if (t >= st[k][0] && t <= st[k + 1][0]) {
            float u = (t - st[k][0]) / (st[k + 1][0] - st[k][0]);
            for (int j = 0; j < 3; j++) out[j] = st[k][j + 1] * (1 - u) + st[k + 1][j + 1] * u;
            return;
        }
    }
}

// las mismas paletas del prototipo del PC (proto/bioma_pc.py)
uint16_t sim_palette(int i) { return s_pal[i < 0 ? 0 : i > 255 ? 255 : i]; }

void sim_build_palette(float day)
{
    static const float noche[5][4] = { {0, 2, 3, 12}, {0.25f, 10, 20, 90}, {0.5f, 0, 150, 220}, {0.75f, 120, 255, 230}, {1, 255, 255, 255} };
    static const float dia[6][4] = { {0, 6, 0, 10}, {0.2f, 70, 0, 120}, {0.45f, 230, 0, 140}, {0.7f, 255, 120, 0}, {0.88f, 255, 225, 90}, {1, 255, 255, 255} };
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f, c1[3] = {0}, c2[3] = {0};
        ramp(noche, 5, t, c1);
        ramp(dia, 6, t, c2);
        s_pal[i] = rgb565(c1[0] * (1 - day) + c2[0] * day, c1[1] * (1 - day) + c2[1] * day, c1[2] * (1 - day) + c2[2] * day);
    }
}

// exposición automática: el percentil 99,5 del histograma que dejó la difusión es el "blanco"
static uint32_t exposure_inv(const uint32_t hist0[64], const uint32_t hist1[64])
{
    uint32_t total = TW * TH, acc = 0, ref_bin = 63;
    for (int b = 63; b >= 0; b--) {
        acc += hist0[b] + hist1[b];
        if (acc > total / 200) { ref_bin = b; break; }
    }
    uint32_t ref = (ref_bin + 1) << 10;
    return (1023u << 16) / ref;            // v*1023/ref sin dividir por píxel
}

SIM_HOT void sim_colorize_rows(uint16_t *img, const uint32_t hist0[64], const uint32_t hist1[64], int y0, int y1)
{
    uint32_t inv = exposure_inv(hist0, hist1);
    uint16_t *o = img + y0 * TW;
    for (int y = y0; y < y1; y++) {
        const uint16_t *row = sim_row[y];
        for (int x = 0; x < TW; x++) {
            uint32_t n = (row[x] * inv) >> 16;
            *o++ = s_pal[s_gam[n > 1023 ? 1023 : n]];
        }
    }
}

void sim_colorize(uint16_t *o, const uint32_t hist0[64], const uint32_t hist1[64])
{
    sim_colorize_rows(o, hist0, hist1, 0, TH);
}

SIM_HOT void sim_gray(uint8_t *o, const uint32_t hist0[64], const uint32_t hist1[64])
{
    uint32_t inv = exposure_inv(hist0, hist1);
    for (int y = 0; y < TH; y++) {
        const uint16_t *row = sim_row[y];
        for (int x = 0; x < TW; x++) {
            uint32_t n = (row[x] * inv) >> 16;
            *o++ = s_gam[n > 1023 ? 1023 : n];
        }
    }
}
