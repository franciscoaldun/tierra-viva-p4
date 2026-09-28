// ntsc_core — la señal NTSC-M 240p en C puro (ver ntsc_core.h). Compila igual en el P4 y en el PC.
#include <string.h>
#include <math.h>
#include "ntsc_core.h"
#include "font.h"

// La retención del DAC (cada muestra se sostiene 70 ns) atenúa la subportadora por sinc(π/4) = 0,9003.
// Se compensa en el burst y en el color por igual: así el burst llega con sus 40 IRE p-p de norma.
#define ZOH_COMP 1.1107207f

// sin(fase·90°) para las 4 muestras de cada ciclo de subportadora
static const int SIN4[4] = { 0, 1, 0, -1 };

void ntsc_cfg_default(ntsc_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->bits = 6;
    // IO16 (bit 0, el menos pesado) ... IO21 (bit 5): cada una más o menos la mitad que la anterior.
    // Valores de cualquier kit barato; host_test/elegir_resistencias.py los eligió entre 18 valores comunes:
    // error medio 5,96 mV (un DAC perfecto de 6 bits daría 5,04 mV) y máximo 1,13 V sobre los 75 ohm de la tele.
    static const float r[6] = { 10000, 4700, 2200, 1000, 510, 270 };
    for (int i = 0; i < 6; i++) c->r[i] = r[i];
    c->r_gpio = 30;
    c->vdd = 3.3f;
    c->r_tele = 75;
    c->setup_ire = 7.5f;
    c->blanco_ire = 100;
    c->croma = 1.0f;
    c->filtro_croma = true;
    c->un_cable = false;
    c->v_alto = 1.2f;
}

void ntsc_init(ntsc_t *n, const ntsc_cfg_t *c)
{
    memset(n, 0, sizeof(*n));
    n->cfg = *c;
    int bits = c->bits < 1 ? 1 : c->bits > NTSC_MAX_BITS ? NTSC_MAX_BITS : c->bits;
    n->cfg.bits = bits;
    n->ncodigos = 1 << bits;
    // Modelo del DAC: cada pin (0 o 3,3 V, con su resistencia de salida) llega por su resistencia a un nodo común,
    // y la tele carga ese nodo con 75 ohm. Nodo = suma(G·V) / (suma(G) + 1/75).
    float g[NTSC_MAX_BITS], gsum = 0;
    for (int i = 0; i < bits; i++) {
        float r = c->r[i] > 1 ? c->r[i] : 1;
        g[i] = 1.0f / (r + c->r_gpio);
        gsum += g[i];
    }
    float gl = 1.0f / (c->r_tele > 1 ? c->r_tele : 1);
    for (int k = 0; k < n->ncodigos; k++) {
        float num = 0;
        for (int i = 0; i < bits; i++) if (k & (1 << i)) num += g[i];
        n->v[k] = c->vdd * num / (gsum + gl);
        if (n->v[k] > n->vmax) n->vmax = n->v[k];
    }
    if (c->un_cable) {                   // sin resistencias: el pin da 0 o v_alto (la tele es el divisor)
        n->cfg.bits = 1;
        n->ncodigos = 2;
        n->v[0] = 0;
        n->v[1] = n->vmax = c->v_alto > 0.2f ? c->v_alto : 0.2f;
    }
    if (n->vmax < 0.05f) n->vmax = 0.05f;
    // Señal estándar: punta de sincronismo 0 V, negro de referencia (blanking) 0,286 V, blanco 1,0 V.
    // Si el DAC no llega a 1 V, todo se achica en proporción (la tele se ajusta con el sincronismo).
    n->escala = n->vmax >= 1.0f ? 1.0f : n->vmax;
    // nivel → código más cercano (el DAC puede no ser parejo: se elige el voltaje real más cercano)
    for (int l = 0; l < NTSC_NIVELES; l++) {
        float t = l * n->vmax / (NTSC_NIVELES - 1);
        int best = 0;
        float bd = 1e9f;
        for (int k = 0; k < n->ncodigos; k++) {
            float d = fabsf(n->v[k] - t);
            if (d < bd) { bd = d; best = k; }
        }
        n->q[l] = (uint8_t)best;
    }
    n->l_sync = 0;   // todos los pines en 0
    n->l_blank = ntsc_nivel_ire(n, 0);
    n->l_black = ntsc_nivel_ire(n, c->setup_ire);
    n->l_white = ntsc_nivel_ire(n, c->blanco_ire);
    float lpire = n->escala / 140.0f / (n->vmax / (NTSC_NIVELES - 1));    // niveles por IRE
    n->l_burst = c->un_cable ? 0 : (int)lrintf(20.0f * lpire * ZOH_COMP);   // sin burst la tele apaga el color
    n->k_y = (int)lrintf((n->l_white - n->l_black) * 256.0f / 255.0f);
    n->k_c = (int)lrintf((n->l_white - n->l_black) * c->croma * ZOH_COMP * 256.0f / 255.0f);
}

int ntsc_nivel_ire(const ntsc_t *n, float ire)
{
    float v = n->escala * (ire + 40.0f) / 140.0f;           // -40 IRE (sincronismo) = 0 V
    int l = (int)lrintf(v / n->vmax * (NTSC_NIVELES - 1));
    return l < 0 ? 0 : l >= NTSC_NIVELES ? NTSC_NIVELES - 1 : l;
}

// ---------------- de niveles a códigos ----------------
// Con resistencias: el código cuyo voltaje real está más cerca. Sin resistencias (1 bit): sigma-delta de orden 2,
// el error se arrastra muestra a muestra; en la punta de sincronismo sale 0 limpio y el error vuelve a cero.
typedef struct { int e1, e2; } sd_t;

static void cuantizar(const ntsc_t *n, uint8_t *dst, const int16_t *lv, int len, sd_t *sd)
{
    if (!n->cfg.un_cable) {
        for (int i = 0; i < len; i++) dst[i] = ntsc_q(n, lv[i]);
        return;
    }
    int e1 = sd->e1, e2 = sd->e2;
    for (int i = 0; i < len; i++) {
        if (lv[i] <= 0) { dst[i] = 0; e1 = e2 = 0; continue; }
        int u = lv[i] + 2 * e1 - e2;
        int y = u >= NTSC_NIVELES / 2 ? NTSC_NIVELES - 1 : 0;
        e2 = e1;
        e1 = u - y;
        if (e1 > 6000) e1 = 6000; else if (e1 < -6000) e1 = -6000;
        dst[i] = y ? 1 : 0;
    }
    sd->e1 = e1; sd->e2 = e2;
}

// Estado inicial del sigma-delta distinto en cada línea: si todas parten igual, un color parejo dibuja
// el mismo patrón en todas las líneas y se ven rayas verticales (medido en el televisor de software).
static sd_t sd_linea(int L)
{
    uint32_t h = (uint32_t)L * 2654435761u;
    h ^= h >> 15;
    sd_t sd = { (int)(h % 2048u) - 1024, 0 };
    return sd;
}

// un pulso de sincronismo que empieza en x0 y dura 'largo' muestras (al 50 %), con flancos suavizados
static void pulso(int16_t *lv, int x0, int largo, int l_sync, int l_mid)
{
    lv[x0] = (int16_t)l_mid;
    for (int x = x0 + 1; x < x0 + largo; x++) lv[x] = (int16_t)l_sync;
    lv[x0 + largo] = (int16_t)l_mid;
}

void ntsc_cuadro_base(const ntsc_t *n, uint8_t *fb)
{
    int16_t lv[NTSC_W];
    sd_t sd = { 0, 0 };
    const int ls = n->l_sync, lb = n->l_blank, lm = (n->l_sync + n->l_blank) / 2;
    for (int L = 0; L < NTSC_H; L++) {
        for (int x = 0; x < NTSC_W; x++) lv[x] = (int16_t)lb;
        if (L < 3 || (L >= 6 && L < 9)) {            // pulsos de ecualización: 2,3 µs, dos por línea
            pulso(lv, 0, 33, ls, lm);
            pulso(lv, 455, 33, ls, lm);
        } else if (L < 6) {                           // sincronismo vertical: 27,1 µs abajo y 4,7 µs arriba, dos por línea
            pulso(lv, 0, 388, ls, lm);
            pulso(lv, 455, 388, ls, lm);
        } else {                                      // línea normal
            pulso(lv, 0, 67, ls, lm);                 // sincronismo horizontal: 4,7 µs
            for (int x = 76; x < 112; x++) {          // burst: 9 ciclos a 180°, 5,3 µs después del sincronismo
                int a = (x < 78 || x >= 110) ? n->l_burst / 2 : n->l_burst;
                lv[x] = (int16_t)(lb - a * SIN4[(2 * L + x) & 3]);
            }
            if (L >= NTSC_IMG_Y0 && L < NTSC_IMG_Y0 + NTSC_IMG_H)       // área activa en negro
                for (int x = 135; x < 889; x++) lv[x] = (int16_t)n->l_black;
        }
        cuantizar(n, fb + L * NTSC_W, lv, NTSC_W, &sd);
    }
}

static inline int ex5(unsigned v) { return (int)((v << 3) | (v >> 2)); }
static inline int ex6(unsigned v) { return (int)((v << 2) | (v >> 4)); }

void ntsc_imagen_rgb565(const ntsc_t *n, uint8_t *fb, const uint16_t *img, int ancho, int alto, int x0,
                        int desde, int hasta)
{
    int16_t yy[NTSC_IMG_PX], uu[NTSC_IMG_PX + 2], vv[NTSC_IMG_PX + 2], lv[NTSC_IMG_W];
    const bool color = !n->cfg.un_cable;
    if (desde < 0) desde = 0;
    if (hasta > NTSC_IMG_H) hasta = NTSC_IMG_H;
    for (int a = desde; a < hasta; a++) {
        const int L = NTSC_IMG_Y0 + a;
        // centro de la línea de TV a en coordenadas de la imagen, en 1/256 de fila
        int sy = ((2 * a + 1) * alto * 128) / NTSC_IMG_H - 128;
        if (sy < 0) sy = 0;
        int r0 = sy >> 8, f = sy & 255;
        if (r0 >= alto - 1) { r0 = alto - 1; f = 0; }
        const uint16_t *p0 = img + r0 * ancho + x0;
        const uint16_t *p1 = f ? p0 + ancho : p0;
        for (int i = 0; i < NTSC_IMG_PX; i++) {
            unsigned c0 = p0[i];
            int R = ex5(c0 >> 11), G = ex6((c0 >> 5) & 63), B = ex5(c0 & 31);
            if (f) {
                unsigned c1 = p1[i];
                R = (R * (256 - f) + ex5(c1 >> 11) * f) >> 8;
                G = (G * (256 - f) + ex6((c1 >> 5) & 63) * f) >> 8;
                B = (B * (256 - f) + ex5(c1 & 31) * f) >> 8;
            }
            int Y = (77 * R + 150 * G + 29 * B + 128) >> 8;          // Y' de la norma (0,299 0,587 0,114)
            yy[i] = (int16_t)Y;
            uu[i + 1] = (int16_t)(((B - Y) * 126) >> 8);             // U = 0,493 (B'-Y')
            vv[i + 1] = (int16_t)(((R - Y) * 225) >> 8);             // V = 0,877 (R'-Y')
        }
        if (color && n->cfg.filtro_croma) {                          // [1 2 1]/4: el color no salta de un píxel al otro
            uu[0] = uu[1]; vv[0] = vv[1];
            uu[NTSC_IMG_PX + 1] = uu[NTSC_IMG_PX]; vv[NTSC_IMG_PX + 1] = vv[NTSC_IMG_PX];
            int pu = uu[0], pv = vv[0];
            for (int i = 1; i <= NTSC_IMG_PX; i++) {
                int cu = uu[i], cv = vv[i];
                uu[i] = (int16_t)((pu + 2 * cu + uu[i + 1] + 2) >> 2);
                vv[i] = (int16_t)((pv + 2 * cv + vv[i + 1] + 2) >> 2);
                pu = cu; pv = cv;
            }
        }
        const int lb = n->l_black, ky = n->k_y, kc = color ? n->k_c : 0;
        for (int i = 0; i < NTSC_IMG_PX; i++) {
            int yl = lb + ((yy[i] * ky) >> 8);
            int ul = (uu[i + 1] * kc) >> 8;
            int vl = (vv[i + 1] * kc) >> 8;
            // la muestra 152+2i cae en la fase 0 si (L+i) es par (V y U positivos) y en la 2 si es impar (negativos)
            if (((L + i) & 1) == 0) { lv[2 * i] = (int16_t)(yl + vl); lv[2 * i + 1] = (int16_t)(yl + ul); }
            else                    { lv[2 * i] = (int16_t)(yl - vl); lv[2 * i + 1] = (int16_t)(yl - ul); }
        }
        sd_t sd = sd_linea(L);
        cuantizar(n, fb + L * NTSC_W + NTSC_IMG_X0, lv, NTSC_IMG_W, &sd);
    }
}

void ntsc_imagen_plana(const ntsc_t *n, uint8_t *fb, float ire)
{
    int16_t lv[NTSC_IMG_W];
    int l = ntsc_nivel_ire(n, ire);
    for (int x = 0; x < NTSC_IMG_W; x++) lv[x] = (int16_t)l;
    for (int a = 0; a < NTSC_IMG_H; a++) {
        sd_t sd = sd_linea(NTSC_IMG_Y0 + a);
        cuantizar(n, fb + (NTSC_IMG_Y0 + a) * NTSC_W + NTSC_IMG_X0, lv, NTSC_IMG_W, &sd);
    }
}

// ---------------- carta de ajuste ----------------
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void rect(uint16_t *img, int x0, int y0, int x1, int y1, uint16_t c)
{
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) img[y * NTSC_IMG_PX + x] = c;
}

static int glifo(uint32_t cp)
{
    for (int i = 0; i < FONT_N; i++) if (font_cp[i] == cp) return i;
    return 0;
}

// texto blanco con la fuente de 12x24 (escala 1 o 2), centrado en cx
static void texto(uint16_t *img, const char *s, int cx, int y0, int esc)
{
    uint32_t cps[64];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < 64; ) {        // UTF-8 mínimo
        uint32_t cp = *p++;
        if (cp >= 0xE0 && p[0] && p[1]) { cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (cp >= 0xC0 && p[0]) { cp = ((cp & 31) << 6) | (p[0] & 63); p += 1; }
        cps[n++] = cp;
    }
    int x = cx - n * FONT_W * esc / 2;
    for (int k = 0; k < n; k++, x += FONT_W * esc) {
        const uint8_t *a = font_a8[glifo(cps[k])];
        for (int gy = 0; gy < FONT_H * esc; gy++) for (int gx = 0; gx < FONT_W * esc; gx++) {
            int px = x + gx, py = y0 + gy;
            if (px < 0 || px >= NTSC_IMG_PX || py < 0 || py >= NTSC_IMG_H) continue;
            int al = a[(gy / esc) * FONT_W + gx / esc];
            if (!al) continue;
            uint16_t c = img[py * NTSC_IMG_PX + px];
            int r = ex5(c >> 11), g = ex6((c >> 5) & 63), b = ex5(c & 31);
            r += ((255 - r) * al) >> 8; g += ((255 - g) * al) >> 8; b += ((255 - b) * al) >> 8;
            img[py * NTSC_IMG_PX + px] = rgb(r, g, b);
        }
    }
}

void ntsc_carta_rgb565(uint16_t *img)
{
    static const uint8_t barras[7][3] = { {191,191,191}, {191,191,0}, {0,191,191}, {0,191,0}, {191,0,191}, {191,0,0}, {0,0,191} };
    static const uint8_t castillos[7][3] = { {0,0,191}, {0,0,0}, {191,0,191}, {0,0,0}, {0,191,191}, {0,0,0}, {191,191,191} };
    for (int i = 0; i < 7; i++) {
        int x0 = i * NTSC_IMG_PX / 7, x1 = (i + 1) * NTSC_IMG_PX / 7;
        rect(img, x0, 0, x1, 150, rgb(barras[i][0], barras[i][1], barras[i][2]));
        rect(img, x0, 150, x1, 170, rgb(castillos[i][0], castillos[i][1], castillos[i][2]));
    }
    for (int x = 0; x < NTSC_IMG_PX; x++) {                  // escala de grises de negro a blanco
        int v = x * 255 / (NTSC_IMG_PX - 1);
        rect(img, x, 170, x + 1, 205, rgb(v, v, v));
    }
    rect(img, 0, 205, NTSC_IMG_PX, NTSC_IMG_H, rgb(0, 0, 0));
    for (int x = 0; x < NTSC_IMG_PX; x++) { img[x] = img[(NTSC_IMG_H - 1) * NTSC_IMG_PX + x] = rgb(255, 255, 255); }
    for (int y = 0; y < NTSC_IMG_H; y++) { img[y * NTSC_IMG_PX] = img[y * NTSC_IMG_PX + NTSC_IMG_PX - 1] = rgb(255, 255, 255); }
    texto(img, "P4 → TELE · NTSC 240p", NTSC_IMG_PX / 2, 210, 1);
    texto(img, "BIOMA", NTSC_IMG_PX / 2, 51, 2);
}
