// Prueba en el PC del generador NTSC (el mismo ntsc_core.c que corre en el P4).
//   ntsc_host carta  salida.bin                     → carta de ajuste
//   ntsc_host imagen entrada.rgb565 ancho alto x0 salida.bin   → una imagen (p. ej. un cuadro de BIOMA de 480x270, x0=60)
//   opcional al final:  r=10000,4700,2200,1000,510,270  setup=7.5  croma=1.0  filtro=1  uncable=1 valto=1.2
// Escribe el cuadro (262 x 910 códigos) y salida.bin.json con el voltaje de cada código (para el televisor de software).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../main/ntsc_core.h"

static uint8_t fb[NTSC_FB];

static void opciones(ntsc_cfg_t *c, int argc, char **argv, int desde)
{
    for (int i = desde; i < argc; i++) {
        if (!strncmp(argv[i], "r=", 2)) {
            int k = 0;
            char *s = argv[i] + 2;
            while (*s && k < NTSC_MAX_BITS) { c->r[k++] = strtof(s, &s); if (*s == ',') s++; }
            c->bits = k;
        } else if (!strncmp(argv[i], "setup=", 6)) c->setup_ire = strtof(argv[i] + 6, NULL);
        else if (!strncmp(argv[i], "croma=", 6)) c->croma = strtof(argv[i] + 6, NULL);
        else if (!strncmp(argv[i], "filtro=", 7)) c->filtro_croma = atoi(argv[i] + 7) != 0;
        else if (!strncmp(argv[i], "uncable=", 8)) c->un_cable = atoi(argv[i] + 8) != 0;
        else if (!strncmp(argv[i], "valto=", 6)) c->v_alto = strtof(argv[i] + 6, NULL);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "uso: ntsc_host carta salida.bin | imagen in.rgb565 ancho alto x0 salida.bin\n"); return 1; }
    ntsc_cfg_t cfg;
    ntsc_cfg_default(&cfg);
    ntsc_t n;
    const char *salida;
    uint16_t *img = NULL;
    int ancho = NTSC_IMG_PX, alto = NTSC_IMG_H, x0 = 0;
    if (!strcmp(argv[1], "carta")) {
        salida = argv[2];
        opciones(&cfg, argc, argv, 3);
        img = calloc(NTSC_IMG_PX * NTSC_IMG_H, 2);
        ntsc_carta_rgb565(img);
    } else if (!strcmp(argv[1], "imagen") && argc >= 7) {
        ancho = atoi(argv[3]); alto = atoi(argv[4]); x0 = atoi(argv[5]); salida = argv[6];
        opciones(&cfg, argc, argv, 7);
        img = malloc((size_t)ancho * alto * 2);
        FILE *f = fopen(argv[2], "rb");
        if (!f || fread(img, 2, (size_t)ancho * alto, f) != (size_t)ancho * alto) { fprintf(stderr, "no pude leer %s\n", argv[2]); return 1; }
        fclose(f);
    } else { fprintf(stderr, "modo desconocido\n"); return 1; }

    ntsc_init(&n, &cfg);
    ntsc_cuadro_base(&n, fb);
    clock_t t0 = clock();
    int vueltas = 50;
    for (int k = 0; k < vueltas; k++) ntsc_imagen_rgb565(&n, fb, img, ancho, alto, x0, 0, NTSC_IMG_H);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / vueltas;

    FILE *f = fopen(salida, "wb");
    fwrite(fb, 1, NTSC_FB, f);
    fclose(f);
    char js[512];
    snprintf(js, sizeof js, "%s.json", salida);
    f = fopen(js, "w");
    fprintf(f, "{\"bits\":%d,\"vmax\":%.5f,\"escala\":%.5f,\"setup\":%.2f,\"niveles\":{\"sync\":%d,\"blank\":%d,\"negro\":%d,\"blanco\":%d,\"burst\":%d},\"v\":[",
            n.cfg.bits, n.vmax, n.escala, n.cfg.setup_ire, n.l_sync, n.l_blank, n.l_black, n.l_white, n.l_burst);
    for (int k = 0; k < n.ncodigos; k++) fprintf(f, "%s%.6f", k ? "," : "", n.v[k]);
    fprintf(f, "]}\n");
    fclose(f);
    printf("cuadro %dx%d → %s | vmax %.3f V escala %.3f | códigos: sync %d blank %d negro %d blanco %d | conversión %.2f ms (PC)\n",
           NTSC_W, NTSC_H, salida, n.vmax, n.escala, ntsc_q(&n, n.l_sync), ntsc_q(&n, n.l_blank), ntsc_q(&n, n.l_black),
           ntsc_q(&n, n.l_white), ms);
    return 0;
}
