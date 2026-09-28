// ntsc_core — la señal de TV antigua (NTSC-M, 240p, video compuesto) en C puro.
// La misma en el P4 (tv.c la saca por los pines) y en el PC (host_test/ la mira con un televisor de software).
//
// Un cuadro = 262 líneas × 910 muestras a 14,318181 MHz (4 veces la subportadora de color, 3,579545 MHz).
// Cada muestra es un código del "DAC" de resistencias (bit 0 = IO16). El cuadro trae TODO lo que la tele necesita:
// sincronismo horizontal y vertical, el "burst" de color y la imagen. El P4 sólo lo repite sin parar por DMA.
//
// Con 4 muestras por ciclo de subportadora, el color es aritmética simple: las 4 fases valen Y+V, Y+U, Y-V, Y-U.
// Una línea tiene 227,5 ciclos, así que la fase se invierte en cada línea (igual que en la norma).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTSC_W       910        // muestras por línea (63,556 µs)
#define NTSC_H       262        // líneas por cuadro (240p: todas las pasadas iguales, como las consolas)
#define NTSC_FB      (NTSC_W * NTSC_H)
#define NTSC_FS_HZ   14318182   // 4 × 3.579.545,45 Hz
#define NTSC_IMG_X0  152        // imagen: 720 muestras (2 por píxel, 360 píxeles), centrada en el área activa
#define NTSC_IMG_W   720
#define NTSC_IMG_PX  360
#define NTSC_IMG_Y0  20         // primera línea con imagen
#define NTSC_IMG_H   240
#define NTSC_NIVELES 4096       // resolución interna de niveles (0 .. voltaje máximo del DAC)
#define NTSC_MAX_BITS 8

typedef struct {
    int   bits;                  // pines del DAC (1..8), bit 0 = el primero (IO16)
    float r[NTSC_MAX_BITS];      // resistencia de cada pin en ohm (la del bit 0 primero)
    float r_gpio;                // resistencia de salida de un pin del P4 (~30 ohm con la fuerza máxima)
    float vdd;                   // 3,3 V
    float r_tele;                // entrada de la tele: 75 ohm
    float setup_ire;             // nivel de negro: 7,5 IRE (NTSC-M) o 0 (NTSC-J)
    float blanco_ire;            // nivel de blanco: 100 IRE
    float croma;                 // saturación: 1 = normal
    bool  filtro_croma;          // suavizar el color entre píxeles vecinos (menos "arcoíris" en bordes finos)
    bool  un_cable;              // modo de prueba SIN resistencias: un solo pin directo a la tele, 1 bit con tramado
                                 // sigma-delta y sin burst (blanco y negro con grano; sirve para probar la tele)
    float v_alto;                // en ese modo: voltaje que da el pin sobre los 75 ohm de la tele (~1,2 V con la fuerza mínima)
} ntsc_cfg_t;

typedef struct {
    ntsc_cfg_t cfg;
    int     ncodigos;
    float   v[1 << NTSC_MAX_BITS];   // voltaje en la tele de cada código (modelo del DAC)
    float   vmax;                    // voltaje del código más alto
    float   escala;                  // 1 = señal estándar de 1 Vpp; menos si el DAC no alcanza
    uint8_t q[NTSC_NIVELES];         // nivel → código más cercano
    int     l_sync, l_blank, l_black, l_white, l_burst;   // niveles internos (0..4095)
    int     k_y, k_c;                // ×256: de 0..255 a niveles (luma) y de U/V a niveles (croma)
} ntsc_t;

void ntsc_cfg_default(ntsc_cfg_t *c);        // 6 pines: 10k/4k7/2k2/1k/510/270 de IO16 a IO21 (lo recomendado)
void ntsc_init(ntsc_t *n, const ntsc_cfg_t *c);
int  ntsc_nivel_ire(const ntsc_t *n, float ire);          // IRE → nivel interno

static inline uint8_t ntsc_q(const ntsc_t *n, int l)
{
    return n->q[l < 0 ? 0 : l >= NTSC_NIVELES ? NTSC_NIVELES - 1 : l];
}

// El cuadro fijo: sincronismos (con pulsos de ecualización y vertical dentados, como la norma), burst y negro.
void ntsc_cuadro_base(const ntsc_t *n, uint8_t *fb);

// Imagen RGB565 → líneas de imagen [linea_desde, linea_hasta) de las 240.
// Toma 360 columnas desde x0 (2 muestras por píxel) y reparte las 'alto' filas en las 240 líneas mezclando vecinas.
void ntsc_imagen_rgb565(const ntsc_t *n, uint8_t *fb, const uint16_t *img, int ancho, int alto, int x0,
                        int linea_desde, int linea_hasta);

// Llenar todas las líneas de imagen de un color de la tabla de niveles (negro, blanco...).
void ntsc_imagen_plana(const ntsc_t *n, uint8_t *fb, float ire);

// Carta de ajuste en RGB565 (360×240): barras de color al 75 %, castillos, escala de grises, marco y texto.
void ntsc_carta_rgb565(uint16_t *img);

#ifdef __cplusplus
}
#endif
