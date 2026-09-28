// TIERRA VIVA — dibujo del globo (C puro: corre igual en el P4 y en el PC).
//
// Por cada píxel del disco: la normal de la esfera (vista) -> coordenadas terrestres con una matriz 3x3 ->
// latitud/longitud -> texturas. La luz sale de la posición real del sol: día con relieve y mar que brilla,
// noche con las luces de las ciudades, crepúsculo naranjo, nubes reales encima y la atmósfera en el borde.
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "tierra.h"

#define PI_F 3.14159265f


static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float smooth(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

// atan2 rápido (error < 0,0015 rad, menos de un cuarto de téxel): la versión de la libc es lo más caro del cuadro
static inline float atan2_r(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx < 1e-20f) return 0;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = PI_F - r;
    return y < 0 ? -r : r;
}

// tramado de Bayer 4x4: reparte el error de pasar a 16 bits y los degradados salen lisos
static const float BAYER[16] = { 0.03f, 0.53f, 0.16f, 0.66f, 0.78f, 0.28f, 0.91f, 0.41f,
                                 0.22f, 0.72f, 0.09f, 0.59f, 0.97f, 0.47f, 0.84f, 0.34f };
static inline uint16_t rgb565d(float r, float g, float b, int x, int y)
{
    float d = BAYER[((y & 3) << 2) | (x & 3)];
    int R = (int)(clampf(r, 0, 1) * 31.0f + d), G = (int)(clampf(g, 0, 1) * 63.0f + d), B = (int)(clampf(b, 0, 1) * 31.0f + d);
    if (R > 31) R = 31;
    if (G > 63) G = 63;
    if (B > 31) B = 31;
    return (uint16_t)((R << 11) | (G << 5) | B);
}

static inline uint16_t rgb565(float r, float g, float b)
{
    int R = (int)(clampf(r, 0, 1) * 31.0f + 0.5f), G = (int)(clampf(g, 0, 1) * 63.0f + 0.5f), B = (int)(clampf(b, 0, 1) * 31.0f + 0.5f);
    return (uint16_t)((R << 11) | (G << 5) | B);
}

void tierra_sol(double unix_utc, float sol[3])
{
    double dias = unix_utc / 86400.0;
    double n = fmod(dias - 10957.5, 365.2422);          // días desde el 1-ene-2000 (mediodía)
    if (n < 0) n += 365.2422;
    double N = n + 1;                                    // día del año aproximado
    double decl = -23.44 * cos(2 * M_PI / 365.0 * (N + 10)) * M_PI / 180.0;
    double B = 2 * M_PI * (N - 81) / 364.0;
    double eot_min = 9.87 * sin(2 * B) - 7.53 * cos(B) - 1.5 * sin(B);   // ecuación del tiempo
    double utc_h = fmod(unix_utc / 3600.0, 24.0);
    double lon = -15.0 * (utc_h - 12.0 + eot_min / 60.0) * M_PI / 180.0;  // longitud subsolar
    sol[0] = (float)(cos(decl) * sin(lon));
    sol[1] = (float)sin(decl);
    sol[2] = (float)(cos(decl) * cos(lon));
}

void tierra_preparar(tierra_t *e)
{
    // vista = Rx(pitch) · Ry(-yaw) · terrestre
    float cy = cosf(e->yaw), sy = sinf(e->yaw), cp = cosf(e->pitch), sp = sinf(e->pitch);
    // Ry(-yaw): x' = x cy - z sy ; z' = x sy + z cy
    // Rx(p):    y'' = y cp - z' sp ; z'' = y sp + z' cp
    float *m = e->m;
    m[0] = cy;        m[1] = 0;   m[2] = -sy;
    m[3] = -sp * sy;  m[4] = cp;  m[5] = -sp * cy;
    m[6] = cp * sy;   m[7] = sp;  m[8] = cp * cy;
    for (int i = 0; i < 3; i++) e->solv[i] = m[i * 3] * e->sol[0] + m[i * 3 + 1] * e->sol[1] + m[i * 3 + 2] * e->sol[2];
    // para la tabla: el sol sin el giro de longitud (Ry(-yaw)·sol) y el giro en dieciseisavos de téxel
    e->solq[0] = e->sol[0] * cy - e->sol[2] * sy;
    e->solq[1] = e->sol[1];
    e->solq[2] = e->sol[0] * sy + e->sol[2] * cy;
    int g = (int)lroundf(e->yaw * (TIERRA_TW * 16 / (2 * PI_F)));
    g %= TIERRA_TW * 16;
    if (g < 0) g += TIERRA_TW * 16;
    e->giro16 = g;
    float hx = e->solv[0], hy = e->solv[1], hz = e->solv[2] + 1.0f;
    float l = sqrtf(hx * hx + hy * hy + hz * hz);
    if (l < 1e-6f) l = 1;
    e->h[0] = hx / l; e->h[1] = hy / l; e->h[2] = hz / l;
}

static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

// asin rápido (Abramowitz-Stegun 4.4.45, error < 7e-5 rad)
static inline float asin_r(float x)
{
    float a = fabsf(x);
    if (a > 1) a = 1;
    float r = 1.5707963f - sqrtf(1.0f - a) * (1.5707288f + a * (-0.2121144f + a * (0.0742610f - 0.0187293f * a)));
    return x < 0 ? -r : r;
}

void tierra_filas(const tierra_t *e, uint16_t *out, int w, int h, int y0, int y1)
{
    const float *m = e->m;
    const float inv_r = 1.0f / e->R;
    const float halo = 1.12f;
    const int tbx = (int)(e->cx - TIERRA_TABLA_LADO / 2), tby = (int)(e->cy - TIERRA_TABLA_LADO / 2);   // esquina de la tabla
    for (int y = y0; y < y1; y++) {
        uint16_t *o = out + (y - y0) * w;                  // out = la fila y0 (puede ser una baldosa en RAM interna)
        float dy = (e->cy - (y + 0.5f)) * inv_r;
        // sólo se recorre el tramo de la fila que cae en el globo o en su halo; el resto es espacio negro
        float q = halo * halo - dy * dy;
        if (q <= 0 || y < e->clip_y0 || y >= e->clip_y1) { memset(o, 0, (size_t)w * 2); continue; }
        float xh = sqrtf(q) * e->R;
        int xa = (int)(e->cx - xh), xb = (int)(e->cx + xh) + 1;
        if (xa < e->clip_x0) xa = e->clip_x0;
        if (xb > e->clip_x1) xb = e->clip_x1;
        if (xb < xa) xb = xa;
        memset(o, 0, (size_t)xa * 2);
        memset(o + xb, 0, (size_t)(w - xb) * 2);
        for (int x = xa; x < xb; x++) {
            float dx = ((x + 0.5f) - e->cx) * inv_r;
            float d2 = dx * dx + dy * dy;
            if (d2 >= halo * halo) { o[x] = 0; continue; }
            if (d2 >= 1.0f) {                              // halo de la atmósfera por fuera del borde
                float r = sqrtf(d2);
                float k = (halo - r) / (halo - 1.0f);
                k = k * k * k;
                float lado = clampf(0.15f + 0.95f * ((dx * e->solv[0] + dy * e->solv[1]) / r) + 0.4f * e->solv[2], 0.08f, 1.0f);
                k *= lado;
                o[x] = rgb565d(0.30f * k, 0.55f * k, 1.0f * k, x, y);
                continue;
            }
            float z, s, fu = 0, fv = 0;
            int ti;
            tierra_px_t *tp = e->tabla ? &e->tabla[(y - tby) * TIERRA_TABLA_LADO + (x - tbx)] : NULL;
            if (tp && !e->tabla_rehacer) {                 // camino rápido: todo sale de la tabla
                z = tp->z * (1.0f / 65535.0f);
                s = (tp->qx * e->solq[0] + tp->qy * e->solq[1] + tp->qz * e->solq[2]) * (1.0f / 16384.0f);
                int u = ((tp->u16 + e->giro16) >> 4) & (TIERRA_TW - 1);
                ti = tp->v * TIERRA_TW + u;
            } else {
                z = sqrtf(1.0f - d2);
                // normal en vista (dx, dy, z) -> terrestre con la transpuesta
                float px = m[0] * dx + m[3] * dy + m[6] * z;
                float py = m[1] * dx + m[4] * dy + m[7] * z;
                float pz = m[2] * dx + m[5] * dy + m[8] * z;
                s = px * e->sol[0] + py * e->sol[1] + pz * e->sol[2];     // coseno del ángulo con el sol
                float lon = atan2_r(px, pz);
                float lat = asin_r(py);                           // (la normal es unitaria)
                float uf = (lon + PI_F) * (TIERRA_TW / (2 * PI_F)), vf = (PI_F * 0.5f - lat) * (TIERRA_TH / PI_F);
                int u = (int)uf, v = (int)vf;
                fu = uf - u; fv = vf - v;
                if (u < 0) u = 0; else if (u >= TIERRA_TW) u = TIERRA_TW - 1;
                if (v < 0) v = 0; else if (v >= TIERRA_TH) v = TIERRA_TH - 1;
                ti = v * TIERRA_TW + u;
                if (tp) {                                  // se arma la tabla: lo mismo, pero sin el giro de longitud
                    float cpt = cosf(e->pitch), spt = sinf(e->pitch);
                    float qx = dx, qy = cpt * dy + spt * z, qz = -spt * dy + cpt * z;
                    float lon0 = atan2_r(qx, qz);
                    int u16 = (int)((lon0 + PI_F) * (TIERRA_TW * 16 / (2 * PI_F)));
                    if (u16 < 0) u16 = 0; else if (u16 >= TIERRA_TW * 16) u16 = TIERRA_TW * 16 - 1;
                    tp->u16 = (uint16_t)u16; tp->v = (int16_t)v;
                    tp->qx = (int16_t)lroundf(qx * 16384); tp->qy = (int16_t)lroundf(qy * 16384); tp->qz = (int16_t)lroundf(qz * 16384);
                    tp->z = (uint16_t)lroundf(z * 65535);
                }
            }
            uint32_t T = e->tex[ti];
            float luz_bil = -1;
            uint16_t c = (uint16_t)T;
            float dr = (c >> 11) * (1.0f / 31.0f), dg = ((c >> 5) & 63) * (1.0f / 63.0f), db = (c & 31) * (1.0f / 31.0f);
            if (e->filtrar) {                          // con zoom: promedio de los 4 téxeles vecinos (sin cuadritos)
                int tu = ti & (TIERRA_TW - 1), tv = ti / TIERRA_TW;
                uint32_t T1 = e->tex[tv * TIERRA_TW + ((tu + 1) & (TIERRA_TW - 1))];
                uint32_t T2 = e->tex[(tv + 1 < TIERRA_TH ? tv + 1 : tv) * TIERRA_TW + tu];
                uint32_t T3 = e->tex[(tv + 1 < TIERRA_TH ? tv + 1 : tv) * TIERRA_TW + ((tu + 1) & (TIERRA_TW - 1))];
                
                float w0 = (1 - fu) * (1 - fv), w1 = fu * (1 - fv), w2 = (1 - fu) * fv, w3 = fu * fv;
                #define CR(t) ((float)(((t) >> 11) & 31) * (1.0f / 31.0f))
                #define CG(t) ((float)(((t) >> 5) & 63) * (1.0f / 63.0f))
                #define CB(t) ((float)((t) & 31) * (1.0f / 31.0f))
                dr = dr * w0 + CR(T1) * w1 + CR(T2) * w2 + CR(T3) * w3;
                dg = dg * w0 + CG(T1) * w1 + CG(T2) * w2 + CG(T3) * w3;
                db = db * w0 + CB(T1) * w1 + CB(T2) * w2 + CB(T3) * w3;
                float nb = ((T >> 16) & 255) * w0 + ((T1 >> 16) & 255) * w1 + ((T2 >> 16) & 255) * w2 + ((T3 >> 16) & 255) * w3;
                T = (T & 0xFF00FFFFu) | ((uint32_t)nb << 16);
                #define LZ(t) ((float)((((t) >> 24) & 15) * (((t) >> 24) & 15)) * (1.0f / 225.0f))
                luz_bil = LZ(T) * w0 + LZ(T1) * w1 + LZ(T2) * w2 + LZ(T3) * w3;
            }
            float nube = clampf(((int)((T >> 16) & 255) - 110) * (1.0f / 150.0f), 0, 0.88f);
            float mar = (T >> 28) * (17.0f / 170.0f);
            float luz = luz_bil >= 0 ? luz_bil : (float)(((T >> 24) & 15) * ((T >> 24) & 15)) * (1.0f / 225.0f);
            float dia = smooth(-0.10f, 0.14f, s);
            // día: relieve iluminado, nubes blancas encima, el sol reflejado en el mar
            float lit = 0.10f + 0.95f * clampf(s * 1.25f, 0, 1);
            float r = dr * lit, g = dg * lit, b = db * lit;
            float nb = nube * 0.92f;
            r += (lit * 1.02f - r) * nb; g += (lit * 1.02f - g) * nb; b += (lit * 1.05f - b) * nb;
            float nh = dx * e->h[0] + dy * e->h[1] + z * e->h[2];
            if (nh > 0 && mar > 0.05f) {
                float sp = nh * nh; sp *= sp; sp *= sp; sp *= sp; sp *= sp; sp *= sp;   // ^64
                sp *= mar * (1.0f - nube) * dia * 0.85f;
                r += sp; g += sp * 0.93f; b += sp * 0.78f;
            }
            // noche: luces de las ciudades (tapadas en parte por las nubes) y un poco de luz de luna
            float ln = luz * (1.0f - nube * 0.75f) * 1.9f;
            float nr = ln * 1.0f + dr * 0.035f + nube * 0.03f, ng = ln * 0.72f + dg * 0.035f + nube * 0.03f, nbl = ln * 0.38f + db * 0.05f + nube * 0.04f;
            r = nr + (r - nr) * dia; g = ng + (g - ng) * dia; b = nbl + (b - nbl) * dia;
            // crepúsculo: franja naranja sobre el terminador
            float tw = 1.0f - fabsf(s + 0.035f) * 14.0f;            // sólo del lado de la noche, angosta
            if (tw > 0) { tw *= tw * 0.16f * (0.35f + 0.65f * nube); r += tw; g += tw * 0.45f; b += tw * 0.15f; }
            // atmósfera: el borde se vuelve azul (más del lado del sol)
            float rim = 1.0f - z; rim = rim * rim * rim * 1.6f;
            if (rim > 1) rim = 1;
            float ad = clampf(s + 0.35f, 0, 1);
            r += (0.30f * ad - r) * rim * 0.6f; g += (0.55f * ad - g) * rim * 0.6f; b += (1.0f * ad - b) * rim * 0.6f;
            o[x] = rgb565d(r, g, b, x, y);
        }
    }
    // estrellas: 420 fijas (siempre las mismas posiciones), que titilan; no tapan el globo ni el halo
    for (uint32_t i = 0; i < 420; i++) {
        uint32_t hs = hash32(i * 2654435761u + 12345u);
        int sx = (int)(hs % (uint32_t)w), sy = (int)((hs >> 9) % (uint32_t)h);
        if (sy < y0 || sy >= y1) continue;
        float dx = (sx + 0.5f - e->cx) * inv_r, dy = (e->cy - (sy + 0.5f)) * inv_r;
        if (dx * dx + dy * dy < halo * halo) continue;
        uint32_t h2 = hash32(hs);
        float b = 0.25f + 0.75f * (h2 & 255) / 255.0f;
        b *= b;
        b *= 0.7f + 0.3f * sinf(e->t * (0.7f + ((h2 >> 8) & 7) * 0.4f) + (float)((h2 >> 12) & 63));
        float tinte = ((h2 >> 20) & 3) * 0.06f;             // algunas más azules, otras más cálidas
        out[(sy - y0) * w + sx] = rgb565(b * (0.92f + tinte), b * 0.93f, b * (1.0f - tinte * 0.5f + 0.05f));
    }
}

int tierra_proyectar(const tierra_t *e, float lat, float lon, float alt, float *sx, float *sy)
{
    float la = lat * (PI_F / 180.0f), lo = lon * (PI_F / 180.0f), k = 1.0f + alt;
    float p[3] = { k * cosf(la) * sinf(lo), k * sinf(la), k * cosf(la) * cosf(lo) };
    const float *m = e->m;
    float vx = m[0] * p[0] + m[1] * p[1] + m[2] * p[2];
    float vy = m[3] * p[0] + m[4] * p[1] + m[5] * p[2];
    float vz = m[6] * p[0] + m[7] * p[1] + m[8] * p[2];
    *sx = e->cx + vx * e->R;
    *sy = e->cy - vy * e->R;
    return vz > 0 || vx * vx + vy * vy > 1.0f;             // delante, o fuera del disco (en órbita, se ve)
}

int tierra_desproyectar(const tierra_t *e, float sx, float sy, float *lat, float *lon)
{
    float dx = (sx - e->cx) / e->R, dy = (e->cy - sy) / e->R, d2 = dx * dx + dy * dy;
    if (d2 >= 1) return 0;
    float z = sqrtf(1 - d2);
    const float *m = e->m;
    float px = m[0] * dx + m[3] * dy + m[6] * z, py = m[1] * dx + m[4] * dy + m[7] * z, pz = m[2] * dx + m[5] * dy + m[8] * z;
    *lon = atan2f(px, pz) * (180.0f / PI_F);
    *lat = asinf(clampf(py, -1, 1)) * (180.0f / PI_F);
    return 1;
}

// ---------------- marcas encima ----------------
static inline void mezclar_px(uint16_t *o, float r, float g, float b, float a)
{
    uint16_t c = *o;
    float R = (c >> 11) / 31.0f, G = ((c >> 5) & 63) / 63.0f, B = (c & 31) / 31.0f;
    *o = rgb565(R + (r - R) * a, G + (g - G) * a, B + (b - B) * a);
}

static inline void sumar_px(uint16_t *o, float r, float g, float b)
{
    uint16_t c = *o;
    *o = rgb565((c >> 11) / 31.0f + r, ((c >> 5) & 63) / 63.0f + g, (c & 31) / 31.0f + b);
}

// anillo suave de radio rr y grosor gr (antialiasing por distancia)
static void anillo(uint16_t *out, int w, int h, float cx, float cy, float rr, float gr, float r, float g, float b, float a)
{
    int x0 = (int)(cx - rr - gr - 1), x1 = (int)(cx + rr + gr + 1), y0 = (int)(cy - rr - gr - 1), y1 = (int)(cy + rr + gr + 1);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < h; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < w; x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float k = 1.0f - fabsf(d - rr) / gr;
            if (k > 0) mezclar_px(&out[y * w + x], r, g, b, k * a);
        }
}

static void punto(uint16_t *out, int w, int h, float cx, float cy, float rad, float r, float g, float b, float a)
{
    int x0 = (int)(cx - rad - 1), x1 = (int)(cx + rad + 1), y0 = (int)(cy - rad - 1), y1 = (int)(cy + rad + 1);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < h; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < w; x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float k = clampf(rad + 0.5f - d, 0, 1);
            if (k > 0) mezclar_px(&out[y * w + x], r, g, b, k * a);
        }
}

void tierra_marcas(const tierra_t *e, uint16_t *out, int w, int h)
{
    // aviones: un punto dorado por avión, movido con su velocidad desde que se midió; con zoom, una rayita de rumbo
    if (e->aviones && e->n_aviones) {
        const float *m = e->m;
        float dt = e->aviones_dt;
        bool cerca = e->R > 190;
        for (int i = 0; i < e->n_aviones; i++) {
            const tierra_avion_t *a = &e->aviones[i];
            float qx = a->p[0] + a->v[0] * dt, qy = a->p[1] + a->v[1] * dt, qz = a->p[2] + a->v[2] * dt;
            float vz = m[6] * qx + m[7] * qy + m[8] * qz;
            if (vz < 0.02f) continue;                            // detrás del globo (o pegado al borde)
            float vx = m[0] * qx + m[1] * qy + m[2] * qz, vy = m[3] * qx + m[4] * qy + m[5] * qz;
            int X = (int)(e->cx + vx * e->R), Y = (int)(e->cy - vy * e->R);
            if (X < e->clip_x0 || X >= e->clip_x1 || Y < e->clip_y0 || Y >= e->clip_y1 || X >= w || Y >= h || X < 0 || Y < 0) continue;
            float b = 0.55f + 0.45f * (a->alt_km > 12 ? 1.0f : a->alt_km / 12.0f);   // más altos, más brillantes
            sumar_px(&out[Y * w + X], 0.95f * b, 0.75f * b, 0.25f * b);
            if (cerca) {                                         // rumbo: 3 píxeles hacia donde va
                float dx = m[0] * a->v[0] + m[1] * a->v[1] + m[2] * a->v[2], dy = -(m[3] * a->v[0] + m[4] * a->v[1] + m[5] * a->v[2]);
                float l = sqrtf(dx * dx + dy * dy);
                if (l > 1e-12f) {
                    dx /= l; dy /= l;
                    for (int k = 1; k <= 3; k++) {
                        int XX = X + (int)lroundf(dx * k), YY = Y + (int)lroundf(dy * k);
                        if (XX >= 0 && XX < w && YY >= 0 && YY < h) sumar_px(&out[YY * w + XX], 0.5f * b, 0.4f * b, 0.12f * b);
                    }
                }
            }
        }
    }
    // sismos: un punto por sismo y ondas que se expanden (más grandes y rápidas cuanto mayor la magnitud)
    for (int i = 0; i < e->n_sismos; i++) {
        const tierra_sismo_t *q = &e->sismos[i];
        float sx, sy;
        if (!tierra_proyectar(e, q->lat, q->lon, 0.0f, &sx, &sy)) continue;
        float dx = (sx - e->cx) / e->R, dy = (e->cy - sy) / e->R;
        if (dx * dx + dy * dy > 0.985f) continue;             // pegado al borde: se ve aplastado, mejor no
        float mg = clampf((q->mag - 2.5f) / 4.5f, 0, 1);       // 2,5 -> 0 ; 7 -> 1
        float fresco = clampf(1.0f - q->edad_h / 24.0f, 0.25f, 1.0f);
        float r = 1.0f, g = 0.85f - 0.7f * mg, b = 0.2f * (1 - mg);
        punto(out, w, h, sx, sy, 0.9f + 2.2f * mg, r, g, b, 0.95f);
        float periodo = 2.6f - 1.2f * mg;
        float fase = fmodf(e->t + i * 0.37f, periodo) / periodo;
        float rmax = 5.0f + 22.0f * mg * mg + 4.0f * mg;
        anillo(out, w, h, sx, sy, 1.5f + fase * rmax, 1.1f + mg, r, g, b, (1.0f - fase) * 0.85f * fresco);
        if (mg > 0.55f) anillo(out, w, h, sx, sy, 1.5f + fmodf(fase + 0.5f, 1.0f) * rmax, 1.0f + mg, r, g, b, (1.0f - fmodf(fase + 0.5f, 1.0f)) * 0.6f);
    }
    // EEI: estela que se desvanece y la estación brillante
    if (e->eei_ok) {
        float alt = e->eei_alt_km / 6371.0f;
        float px = 0, py = 0;
        int hay = 0;
        for (int i = 0; i < e->n_estela; i++) {
            float sx, sy;
            int ve = tierra_proyectar(e, e->estela[i][0], e->estela[i][1], alt, &sx, &sy);
            float a = (float)(i + 1) / (e->n_estela + 1);
            if (ve && hay) {                                  // segmento desde el punto anterior
                float ddx = sx - px, ddy = sy - py, L = sqrtf(ddx * ddx + ddy * ddy);
                if (L < 60) {
                    int n = (int)L + 1;
                    for (int k = 0; k <= n; k++) {
                        int X = (int)(px + ddx * k / n), Y = (int)(py + ddy * k / n);
                        if (X >= 0 && X < w && Y >= 0 && Y < h) sumar_px(&out[Y * w + X], 0.15f * a, 0.55f * a, 0.65f * a);
                    }
                }
            }
            px = sx; py = sy; hay = ve;
        }
        float sx, sy;
        if (tierra_proyectar(e, e->eei_lat, e->eei_lon, alt, &sx, &sy)) {
            float lat_ = 0.6f + 0.4f * sinf(e->t * 6.0f);
            punto(out, w, h, sx, sy, 5.0f, 0.3f, 0.9f, 1.0f, 0.25f * lat_);
            punto(out, w, h, sx, sy, 2.2f, 0.85f, 1.0f, 1.0f, 1.0f);
            // paneles solares: una rayita a cada lado
            for (int k = -5; k <= 5; k++) if (k < -2 || k > 2) {
                int X = (int)sx + k, Y = (int)sy;
                if (X >= 0 && X < w && Y >= 0 && Y < h) mezclar_px(&out[Y * w + X], 0.55f, 0.75f, 1.0f, 0.9f);
            }
        }
    }
}

// ---------------- aviones ----------------
// cada estado de OpenSky: [icao, callsign, país, t_pos, t_contacto, lon, lat, alt_baro, en_tierra, velocidad, rumbo, ...]
static const char *campo(const char *p, int n)      // salta n comas dentro del arreglo del avión (respetando comillas)
{
    for (int k = 0; k < n && *p && *p != ']'; p++) {
        if (*p == '"') { p++; while (*p && *p != '"') p++; }
        else if (*p == ',') k++;
    }
    return p;
}

int tierra_leer_aviones(const char *json, tierra_avion_t *out, int max, double *t_datos)
{
    const char *p = strstr(json, "\"time\":");
    *t_datos = p ? strtod(p + 7, NULL) : 0;
    p = strstr(json, "\"states\":[");
    if (!p) return 0;
    p += 10;
    int n = 0;
    while (n < max) {
        p = strchr(p, '[');
        if (!p) break;
        const char *e = strchr(p, ']');               // fin de este avión (no hay corchetes adentro)
        if (!e) break;
        const char *f = campo(p + 1, 5);
        char *z;
        double lon = strtod(f, &z);
        if (z == f) { p = e + 1; continue; }            // null: sin posición
        f = campo(p + 1, 6); double lat = strtod(f, &z);
        if (z == f) { p = e + 1; continue; }
        f = campo(p + 1, 7); double alt = strtod(f, &z); if (z == f) alt = 0;
        f = campo(p + 1, 8);
        if (strncmp(f, "true", 4) == 0) { p = e + 1; continue; }   // en tierra: no vuela
        f = campo(p + 1, 9); double vel = strtod(f, &z); if (z == f) vel = 0;
        f = campo(p + 1, 10); double rumbo = strtod(f, &z); if (z == f) rumbo = 0;
        float la = (float)(lat * M_PI / 180), lo = (float)(lon * M_PI / 180), th = (float)(rumbo * M_PI / 180);
        float cla = cosf(la), sla = sinf(la), clo = cosf(lo), slo = sinf(lo);
        tierra_avion_t *a = &out[n++];
        a->p[0] = cla * slo; a->p[1] = sla; a->p[2] = cla * clo;
        // este = (cos lon, 0, -sin lon) ; norte = (-sin lat sin lon, cos lat, -sin lat cos lon)
        float k = (float)(vel / 6371000.0), se = sinf(th) * k, sn = cosf(th) * k;
        a->v[0] = se * clo - sn * sla * slo;
        a->v[1] = sn * cla;
        a->v[2] = -se * slo - sn * sla * clo;
        a->alt_km = (float)(alt / 1000.0);
        p = e + 1;
    }
    return n;
}
