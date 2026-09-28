// BIOMA — reloj futurista (ver bioma_reloj.h).
// Números de trazo fino con esquinas cortadas a 45° (estilo "sci-fi"), dibujados como distancia a segmentos:
// núcleo casi blanco + brillo del color de la paleta. Las máscaras se recalculan sólo cuando cambia lo que muestran:
// hora y minutos una vez por minuto; los segundos una vez por segundo (un recuadro chico).
#include <math.h>
#include <string.h>
#include "bioma_sim.h"
#include "bioma_reloj.h"

// espacio de diseño de un glifo: 36 x 64, esquina cortada de 9
#define GW 36.0f
#define GH 64.0f
#define GC 9.0f
#define GM 32.0f

typedef struct { float x, y; } pt_t;
typedef struct { const pt_t *p; int n; } linea_t;
typedef struct { linea_t l[3]; int n; } glifo_t;

#define P(...) (const pt_t[]){ __VA_ARGS__ }
#define L(arr) { arr, (int)(sizeof(arr) / sizeof(pt_t)) }

static const pt_t g0[] = { {GC,0},{GW-GC,0},{GW,GC},{GW,GH-GC},{GW-GC,GH},{GC,GH},{0,GH-GC},{0,GC},{GC,0} };
static const pt_t g0b[] = { {GW-11,15},{11,GH-15} };                              // barra del cero (look técnico)
static const pt_t g1[] = { {GW/2-9,9},{GW/2,0},{GW/2,GH} };
static const pt_t g2[] = { {0,GC},{GC,0},{GW-GC,0},{GW,GC},{GW,GM-GC},{GW-GC,GM},{GC,GM},{0,GM+GC},{0,GH},{GW,GH} };
static const pt_t g3[] = { {0,0},{GW-GC,0},{GW,GC},{GW,GH-GC},{GW-GC,GH},{0,GH} };
static const pt_t g3b[] = { {10,GM},{GW,GM} };
static const pt_t g4[] = { {0,0},{0,GM+6},{GW,GM+6} };
static const pt_t g4b[] = { {GW-9,10},{GW-9,GH} };
static const pt_t g5[] = { {GW,0},{0,0},{0,GM},{GW-GC,GM},{GW,GM+GC},{GW,GH-GC},{GW-GC,GH},{0,GH} };
static const pt_t g6[] = { {GW,0},{GC,0},{0,GC},{0,GH-GC},{GC,GH},{GW-GC,GH},{GW,GH-GC},{GW,GM+GC},{GW-GC,GM},{0,GM} };
static const pt_t g7[] = { {0,0},{GW-GC,0},{GW,GC},{GW,GH} };
static const pt_t g8[] = { {GC,0},{GW-GC,0},{GW,GC},{GW,GM-GC},{GW-GC,GM},{GW,GM+GC},{GW,GH-GC},{GW-GC,GH},{GC,GH},
                           {0,GH-GC},{0,GM+GC},{GC,GM},{0,GM-GC},{0,GC},{GC,0} };
static const pt_t g8b[] = { {GC,GM},{GW-GC,GM} };
static const pt_t g9[] = { {0,GH},{GW-GC,GH},{GW,GH-GC},{GW,GC},{GW-GC,0},{GC,0},{0,GC},{0,GM-GC},{GC,GM},{GW,GM} };
static const pt_t gA[] = { {0,GH},{0,GC},{GC,0},{GW-GC,0},{GW,GC},{GW,GH} };
static const pt_t gAb[] = { {0,GM+4},{GW,GM+4} };
static const pt_t gP[] = { {0,GH},{0,0},{GW-GC,0},{GW,GC},{GW,GM-GC},{GW-GC,GM},{0,GM} };
static const pt_t gM[] = { {0,GH},{0,0},{GW,0},{GW,GH} };
static const pt_t gMb[] = { {GW/2,0},{GW/2,GM+4} };

static const glifo_t DIG[10] = {
    { { L(g0), L(g0b) }, 2 }, { { L(g1) }, 1 }, { { L(g2) }, 1 }, { { L(g3), L(g3b) }, 2 }, { { L(g4), L(g4b) }, 2 },
    { { L(g5) }, 1 }, { { L(g6) }, 1 }, { { L(g7) }, 1 }, { { L(g8), L(g8b) }, 2 }, { { L(g9) }, 1 },
};
static const glifo_t LET_A = { { L(gA), L(gAb) }, 2 }, LET_P = { { L(gP) }, 1 }, LET_M = { { L(gM), L(gMb) }, 2 };

// ---- geometría de la pantalla (celdas de 480x270; cada celda son 4x4 píxeles a 1080p) ----
#define GLOW 9                          // margen para el brillo
#define BIG_S 1.0f                      // escala de los dígitos grandes (36x64)
#define BIG_T 5.0f                      // grosor del trazo grande
#define SML_S 0.45f                     // segundos (16x29)
#define SML_T 3.0f
#define LET_S 0.34f                     // AM/PM (12x22)
#define LET_T 2.6f
#define ADV 48.0f                       // avance entre dígitos grandes
#define COLON 20.0f                     // ancho de los dos puntos
#define GAP_R 15.0f                     // separación hasta el bloque derecho
#define MAXW 280
#define MAXH 110

static uint8_t s_big[MAXH * MAXW];      // hora + minutos + AM/PM (cambia 1 vez por minuto)
static uint8_t s_sec[64 * 64];          // segundos (cambia 1 vez por segundo)
static int s_key_big = -1, s_key_sec = -1;
static int s_bx, s_by, s_bw, s_bh;      // recuadro de s_big en la imagen
static int s_sx, s_sy, s_sw, s_sh;      // recuadro de s_sec
static float s_colon_x, s_x0, s_width;  // para los dos puntos y el segundero
static const float s_y0 = (TH - GH) / 2.0f - 6.0f;

static float dist_seg(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float c = (wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-6f);
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    float dx = wx - c * vx, dy = wy - c * vy;
    return sqrtf(dx * dx + dy * dy);
}

// intensidad por distancia al borde del trazo: núcleo con borde suave + brillo que se apaga.
// En TABLA: con expf() por píxel el reloj costaba 22 ms por cuadro en el P4 (medido); con la tabla, ~2 ms.
#define ILUT_N 512                       // distancias de 0 a 32 celdas en pasos de 1/16
static uint8_t s_ilut[ILUT_N];
static int s_ilut_ok;

static void ilut_init(void)
{
    for (int i = 0; i < ILUT_N; i++) {
        float d = i / 16.0f, v = d < 1.0f ? 255 - 120 * d : 135.0f * expf(-(d - 1.0f) / 2.8f);
        s_ilut[i] = (uint8_t)(v < 1 ? 0 : v > 255 ? 255 : v);
    }
    s_ilut_ok = 1;
}

static inline uint8_t intensidad(float d, float fuerza)
{
    if (d <= 0.0f) return (uint8_t)(255 * fuerza);
    int i = (int)(d * 16.0f);
    return i >= ILUT_N ? 0 : (uint8_t)(s_ilut[i] * fuerza);
}

// dibuja un glifo en una máscara (máximo con lo que ya había), recortando a su recuadro
static void glifo_en(uint8_t *m, int mw, int mh, int mx0, int my0, const glifo_t *g, float ox, float oy, float s, float t, float fuerza)
{
    int xa = (int)floorf(ox - t - GLOW) - mx0, xb = (int)ceilf(ox + GW * s + t + GLOW) - mx0;
    int ya = (int)floorf(oy - t - GLOW) - my0, yb = (int)ceilf(oy + GH * s + t + GLOW) - my0;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > mw) xb = mw;
    if (yb > mh) yb = mh;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            float px = mx0 + x + 0.5f, py = my0 + y + 0.5f, d = 1e9f;
            for (int k = 0; k < g->n; k++)
                for (int i = 0; i + 1 < g->l[k].n; i++) {
                    const pt_t *a = &g->l[k].p[i], *b = &g->l[k].p[i + 1];
                    float e = dist_seg(px, py, ox + a->x * s, oy + a->y * s, ox + b->x * s, oy + b->y * s);
                    if (e < d) d = e;
                }
            uint8_t v = intensidad(d - t / 2, fuerza);
            if (v > m[y * mw + x]) m[y * mw + x] = v;
        }
}

void reloj_preparar(int hh24, int mm, int ss)
{
    if (!s_ilut_ok) ilut_init();
    int h12 = hh24 % 12 == 0 ? 12 : hh24 % 12;
    int pm = hh24 >= 12;
    int key = h12 * 1000 + mm * 10 + pm;
    if (key != s_key_big) {
        s_key_big = key;
        int nd = h12 >= 10 ? 2 : 1;
        float wbig = (nd - 1) * ADV + GW + COLON + ADV + GW;             // H(H):MM
        float wright = 2 * GW * SML_S + 6;                               // bloque derecho (segundos, el más ancho)
        s_width = wbig + GAP_R + wright;
        s_x0 = (TW - s_width) / 2.0f;
        s_bx = (int)floorf(s_x0) - GLOW - 2; s_by = (int)floorf(s_y0) - GLOW - 2;
        s_bw = (int)ceilf(s_width) + 2 * GLOW + 4; s_bh = (int)GH + 2 * GLOW + 4;
        if (s_bw > MAXW) s_bw = MAXW;
        if (s_bh > MAXH) s_bh = MAXH;
        memset(s_big, 0, sizeof(s_big));
        float x = s_x0;
        if (nd == 2) { glifo_en(s_big, s_bw, s_bh, s_bx, s_by, &DIG[h12 / 10], x, s_y0, BIG_S, BIG_T, 1.0f); x += ADV; }
        glifo_en(s_big, s_bw, s_bh, s_bx, s_by, &DIG[h12 % 10], x, s_y0, BIG_S, BIG_T, 1.0f);
        x += GW;
        s_colon_x = x + COLON / 2.0f;
        x += COLON;
        glifo_en(s_big, s_bw, s_bh, s_bx, s_by, &DIG[mm / 10], x, s_y0, BIG_S, BIG_T, 1.0f);
        x += ADV;
        glifo_en(s_big, s_bw, s_bh, s_bx, s_by, &DIG[mm % 10], x, s_y0, BIG_S, BIG_T, 1.0f);
        x += GW + GAP_R;
        // AM/PM arriba a la derecha, un poco más tenue
        glifo_en(s_big, s_bw, s_bh, s_bx, s_by, pm ? &LET_P : &LET_A, x, s_y0 + 2, LET_S, LET_T, 0.85f);
        glifo_en(s_big, s_bw, s_bh, s_bx, s_by, &LET_M, x + GW * LET_S + 5, s_y0 + 2, LET_S, LET_T, 0.85f);
        // el recuadro de los segundos (abajo a la derecha)
        s_sx = (int)floorf(x) - GLOW; s_sy = (int)floorf(s_y0 + GH - GH * SML_S) - GLOW;
        s_sw = (int)ceilf(wright) + 2 * GLOW; s_sh = (int)ceilf(GH * SML_S) + 2 * GLOW;
        if (s_sw > 64) s_sw = 64;
        if (s_sh > 64) s_sh = 64;
        s_key_sec = -1;
    }
    if (ss != s_key_sec) {
        s_key_sec = ss;
        memset(s_sec, 0, sizeof(s_sec));
        float x = s_sx + GLOW, y = s_sy + GLOW;
        glifo_en(s_sec, s_sw, s_sh, s_sx, s_sy, &DIG[ss / 10], x, y, SML_S, SML_T, 0.95f);
        glifo_en(s_sec, s_sw, s_sh, s_sx, s_sy, &DIG[ss % 10], x + GW * SML_S + 6, y, SML_S, SML_T, 0.95f);
    }
}

static inline uint16_t sumar(uint16_t p, int r, int g, int b)
{
    int R = (((p >> 11) & 31) << 3) + r, G = (((p >> 5) & 63) << 2) + g, B = ((p & 31) << 3) + b;
    R = R > 255 ? 255 : R; G = G > 255 ? 255 : G; B = B > 255 ? 255 : B;
    return (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
}

// núcleo casi blanco; el brillo toma el color de la paleta del momento (noche cian, día magenta/oro)
static inline void pintar_px(uint16_t *px, int v, int gr, int gg, int gb)
{
    int w = v > 200 ? (v - 200) * 255 / 55 : 0;
    int r = (gr * (255 - w) + 255 * w) >> 8, g = (gg * (255 - w) + 250 * w) >> 8, b = (gb * (255 - w) + 240 * w) >> 8;
    *px = sumar(*px, r * v >> 8, g * v >> 8, b * v >> 8);
}

static void punto(uint16_t *img, float cx, float cy, float t, int gr, int gg, int gb, float fuerza)
{
    for (int y = (int)(cy - t - GLOW); y <= (int)(cy + t + GLOW); y++)
        for (int x = (int)(cx - t - GLOW); x <= (int)(cx + t + GLOW); x++) {
            if (x < 0 || x >= TW || y < 0 || y >= TH) continue;
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            uint8_t v = intensidad(sqrtf(dx * dx + dy * dy) - t / 2, fuerza);
            if (v) pintar_px(&img[y * TW + x], v, gr, gg, gb);
        }
}

void reloj_dibujar(uint16_t *img, uint16_t brillo565, int dos_puntos, int ms_minuto)
{
    if (s_key_big < 0) return;
    int gr = ((brillo565 >> 11) & 31) << 3, gg = ((brillo565 >> 5) & 63) << 2, gb = (brillo565 & 31) << 3;
    for (int y = 0; y < s_bh; y++) {
        int iy = s_by + y;
        if (iy < 0 || iy >= TH) continue;
        for (int x = 0; x < s_bw; x++) {
            int ix = s_bx + x;
            if (ix < 0 || ix >= TW) continue;
            int v = s_big[y * s_bw + x];
            int sx = ix - s_sx, sy = iy - s_sy;                              // los segundos, encima
            if (sx >= 0 && sx < s_sw && sy >= 0 && sy < s_sh && s_sec[sy * s_sw + sx] > v) v = s_sec[sy * s_sw + sx];
            if (v) pintar_px(&img[iy * TW + ix], v, gr, gg, gb);
        }
    }
    if (dos_puntos) {                                                        // los dos puntos parpadean cada segundo
        punto(img, s_colon_x, s_y0 + GH * 0.30f, BIG_T + 1, gr, gg, gb, 1.0f);
        punto(img, s_colon_x, s_y0 + GH * 0.72f, BIG_T + 1, gr, gg, gb, 1.0f);
    }
    // segundero: una línea bajo la hora que se llena durante el minuto (con una guía tenue del largo completo)
    float yb = s_y0 + GH + 11.0f, len = s_width * (ms_minuto / 60000.0f);
    for (int y = (int)(yb - 1.5f - GLOW); y <= (int)(yb + 1.5f + GLOW); y++) {
        if (y < 0 || y >= TH) continue;
        for (int x = (int)(s_x0 - GLOW); x <= (int)(s_x0 + s_width + GLOW); x++) {
            if (x < 0 || x >= TW) continue;
            float px = x + 0.5f, py = y + 0.5f;
            float dg = dist_seg(px, py, s_x0, yb, s_x0 + s_width, yb) - 0.6f;           // guía
            int v = intensidad(dg, 0.22f);
            if (len > 0.5f) {
                float dl = dist_seg(px, py, s_x0, yb, s_x0 + len, yb) - 1.4f;            // progreso
                int w = intensidad(dl, 1.0f);
                if (w > v) v = w;
            }
            if (v) pintar_px(&img[y * TW + x], v, gr, gg, gb);
        }
    }
}
