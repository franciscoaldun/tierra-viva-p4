// Prueba en el PC del dibujo de TIERRA VIVA: mismo tierra.c que corre en el P4.
// uso: ./prueba <unix_utc> <lon_centro> <lat_centro> salida.ppm
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../tv-fw/main/tierra.h"

static void *leer(const char *f, size_t n)
{
    FILE *fp = fopen(f, "rb");
    if (!fp) { perror(f); exit(1); }
    void *p = malloc(n);
    if (fread(p, 1, n, fp) != n) { fprintf(stderr, "corto: %s\n", f); exit(1); }
    fclose(fp);
    return p;
}

int main(int argc, char **argv)
{
    static tierra_t e;
    const uint16_t *dia = leer("../dia565.bin", TIERRA_TW * TIERRA_TH * 2);
    const uint8_t *luces = leer("../luces8.bin", TIERRA_TW * TIERRA_TH), *mar = leer("../mar8.bin", TIERRA_TW * TIERRA_TH), *nub = leer("../nubes8.bin", TIERRA_TW * TIERRA_TH);
    uint32_t *tex = malloc(TIERRA_TW * TIERRA_TH * 4);
    for (int i = 0; i < TIERRA_TW * TIERRA_TH; i++) tex[i] = tierra_texel(dia[i], nub[i], luces[i], mar[i]);
    e.tex = tex;
    double t = argc > 1 ? atof(argv[1]) : (double)time(NULL);
    float lon = argc > 2 ? atof(argv[2]) : -71.0f, lat = argc > 3 ? atof(argv[3]) : -30.0f;
    e.yaw = lon * 3.14159265f / 180; e.pitch = lat * 3.14159265f / 180;
    e.cx = 240; e.cy = 135; e.R = argc > 5 ? atof(argv[5]) : 106; e.t = 1.3f; e.clip_x1 = 480; e.clip_y1 = 270; e.filtrar = e.R > 150;
    tierra_sol(t, e.sol);
    // sismos de ejemplo (en el P4 vienen del USGS)
    float q[][3] = { { -33.4f, -71.6f, 5.8f }, { -20.2f, -69.1f, 4.1f }, { -38.0f, -73.5f, 3.2f }, { 35.7f, 140.0f, 6.3f }, { 19.4f, -155.3f, 3.0f }, { -6.0f, -77.0f, 4.6f } };
    e.n_sismos = 6;
    for (int i = 0; i < 6; i++) { e.sismos[i].lat = q[i][0]; e.sismos[i].lon = q[i][1]; e.sismos[i].mag = q[i][2]; e.sismos[i].edad_h = i * 3; }
    e.eei_ok = 1; e.eei_lat = -12; e.eei_lon = -60; e.eei_alt_km = 420;
    e.n_estela = 40;
    for (int i = 0; i < 40; i++) { e.estela[i][0] = -12 - (40 - i) * 0.9f; e.estela[i][1] = -60 - (40 - i) * 1.6f; }
    // aviones reales (respuesta de OpenSky guardada)
    FILE *fa = fopen("../aviones_muestra.json", "rb");
    if (fa) {
        fseek(fa, 0, SEEK_END); long n = ftell(fa); fseek(fa, 0, SEEK_SET);
        char *js = malloc(n + 1); fread(js, 1, n, fa); js[n] = 0; fclose(fa);
        static tierra_avion_t av[TIERRA_MAX_AVIONES];
        double td;
        clock_t c1 = clock();
        e.n_aviones = tierra_leer_aviones(js, av, TIERRA_MAX_AVIONES, &td);
        fprintf(stderr, "aviones en vuelo: %d (hora %.0f), leídos en %.1f ms\n", e.n_aviones, td, (clock() - c1) * 1000.0 / CLOCKS_PER_SEC);
        e.aviones = av; e.aviones_dt = 0;
    }
    tierra_preparar(&e);
    static uint16_t img[480 * 270];
    clock_t c0 = clock();
    for (int k = 0; k < 20; k++) tierra_filas(&e, img, 480, 270, 0, 270);
    tierra_marcas(&e, img, 480, 270);
    fprintf(stderr, "PC: %.2f ms por cuadro\n", (clock() - c0) * 1000.0 / CLOCKS_PER_SEC / 20);
    FILE *fp = fopen(argc > 4 ? argv[4] : "salida.ppm", "wb");
    fprintf(fp, "P6 480 270 255\n");
    for (int i = 0; i < 480 * 270; i++) {
        uint16_t c = img[i];
        unsigned char px[3] = { (unsigned char)((c >> 11) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31) };
        fwrite(px, 1, 3, fp);
    }
    fclose(fp);
    return 0;
}
