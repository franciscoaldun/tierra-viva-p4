// Verifica que el camino con tabla da la misma imagen que el cálculo completo (y cuánto más rápido es).
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../tv-fw/main/tierra.h"
static void *leer(const char *f, size_t n) { FILE *fp = fopen(f, "rb"); void *p = malloc(n); if (!fp || fread(p, 1, n, fp) != n) exit(1); fclose(fp); return p; }
int main(void)
{
    static tierra_t e;
    const uint16_t *dia = leer("../dia565.bin", TIERRA_TW * TIERRA_TH * 2);
    const uint8_t *lu = leer("../luces8.bin", TIERRA_TW * TIERRA_TH), *ma = leer("../mar8.bin", TIERRA_TW * TIERRA_TH), *nu = leer("../nubes8.bin", TIERRA_TW * TIERRA_TH);
    uint32_t *tex = malloc(TIERRA_TW * TIERRA_TH * 4);
    for (int i = 0; i < TIERRA_TW * TIERRA_TH; i++) tex[i] = tierra_texel(dia[i], nu[i], lu[i], ma[i]);
    e.clip_x1 = 480; e.clip_y1 = 270; e.tex = tex; e.cx = 240; e.cy = 135; e.R = 106; e.pitch = -0.4f; e.t = 1;
    tierra_sol(1790000000.0, e.sol);
    static uint16_t ref[480 * 270], con[480 * 270];
    e.tabla = malloc(TIERRA_TABLA_BYTES);
    e.yaw = 0.3f; e.tabla_rehacer = 1; tierra_preparar(&e); tierra_filas(&e, con, 480, 270, 0, 270);   // arma la tabla
    e.yaw = -1.234f; e.tabla_rehacer = 0; tierra_preparar(&e);
    clock_t c0 = clock(); for (int k = 0; k < 50; k++) tierra_filas(&e, con, 480, 270, 0, 270); double tt = (clock() - c0) * 1000.0 / CLOCKS_PER_SEC / 50;
    tierra_px_t *tb = e.tabla; e.tabla = NULL;
    c0 = clock(); for (int k = 0; k < 50; k++) tierra_filas(&e, ref, 480, 270, 0, 270); double tr = (clock() - c0) * 1000.0 / CLOCKS_PER_SEC / 50;
    int dif = 0, grande = 0;
    for (int i = 0; i < 480 * 270; i++) if (ref[i] != con[i]) { dif++; int a = ref[i] >> 11, b = con[i] >> 11; if (abs(a - b) > 3) grande++; }
    printf("píxeles distintos: %d de %d (con diferencia visible: %d) · completo %.2f ms, con tabla %.2f ms\n", dif, 480 * 270, grande, tr, tt);
    FILE *fp = fopen("tabla.ppm", "wb"); fprintf(fp, "P6 960 270 255\n");
    for (int y = 0; y < 270; y++) for (int x = 0; x < 960; x++) { uint16_t c = x < 480 ? ref[y * 480 + x] : con[y * 480 + x - 480]; unsigned char p[3] = { (c >> 11) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31 }; fwrite(p, 1, 3, fp); }
    fclose(fp); (void)tb;
    return 0;
}
