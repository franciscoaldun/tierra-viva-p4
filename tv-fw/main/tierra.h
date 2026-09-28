// TIERRA VIVA — el globo terráqueo con la luz del sol de este instante, las nubes reales, los sismos y la EEI.
// Núcleo del dibujo SIN nada del ESP-IDF: el mismo código corre en el P4 y en el PC (tierra/host_test).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIERRA_TW 1024          // texturas equirrectangulares 1024x512
#define TIERRA_TH 512
#define TIERRA_MAX_SISMOS 160
#define TIERRA_MAX_ESTELA 90
// luz4 = raíz de la luz en 4 bits (así las ciudades chicas y tenues no se pierden), mar4 = máscara de mar en 4 bits
#define TIERRA_TEXEL(dia, nube, luz4, mar4) ((uint32_t)(dia) | ((uint32_t)(nube) << 16) | ((uint32_t)(luz4) << 24) | ((uint32_t)(mar4) << 28))
static inline uint32_t tierra_texel(uint16_t dia, uint8_t nube, uint8_t luz, uint8_t mar)
{
    int l4 = 0;
    while (l4 < 15 && (l4 + 1) * (l4 + 1) * 255 <= luz * 225 + 112) l4++;      // l4 ≈ 15·sqrt(luz/255)
    return TIERRA_TEXEL(dia, nube, l4, mar >> 4);
}

typedef struct { float lat, lon, mag; float edad_h; } tierra_sismo_t;   // grados, magnitud, horas desde que ocurrió

// avión: posición (vector unitario) cuando se midió y velocidad en radios terrestres por segundo, para moverlo
// entre descargas (las posiciones se renuevan cada ~15 min, pero cada avión sigue volando con su rumbo y velocidad)
typedef struct { float p[3], v[3]; float alt_km; } tierra_avion_t;
#define TIERRA_MAX_AVIONES 12000
// lee la respuesta de OpenSky (/api/states/all). Devuelve cuántos aviones en vuelo leyó; *t_datos = su hora unix.
int tierra_leer_aviones(const char *json, tierra_avion_t *out, int max, double *t_datos);

// Tabla por píxel del disco. Mientras el globo sólo gira de lado (longitud), la latitud de cada píxel y su
// posición sin el giro no cambian: se guardan acá y cada cuadro sólo suma el giro. Sin raíces, atan2 ni divisiones.
typedef struct { uint16_t u16; int16_t v; int16_t qx, qy, qz; uint16_t z; } tierra_px_t;   // 12 bytes
#define TIERRA_TABLA_LADO 228          // cubre un globo de radio hasta 112 px
#define TIERRA_TABLA_BYTES (TIERRA_TABLA_LADO * TIERRA_TABLA_LADO * (int)sizeof(tierra_px_t))   // grados, magnitud, horas desde que ocurrió

typedef struct {
    // texturas (PSRAM en el P4)
    // las 4 texturas juntas en un téxel de 32 bits (una sola lectura de PSRAM por píxel, no cuatro):
    //   bits 0-15 día RGB565 · 16-23 nubes · 24-27 luces de ciudades · 28-31 mar
    const uint32_t *tex;
    // vista
    float yaw, pitch;           // longitud y latitud del centro de la vista (rad)
    float cx, cy, R;            // centro y radio del globo en píxeles
    float sol[3];               // dirección al sol, coordenadas terrestres (y = polo norte, z = lon 0, x = lon 90E)
    float t;                    // segundos (animaciones)
    // datos vivos
    int n_sismos;
    tierra_sismo_t sismos[TIERRA_MAX_SISMOS];
    const tierra_avion_t *aviones;
    int n_aviones;
    float aviones_dt;           // segundos desde que se midieron las posiciones
    int eei_ok;                 // hay posición de la Estación Espacial
    float eei_lat, eei_lon, eei_alt_km;
    int n_estela;
    float estela[TIERRA_MAX_ESTELA][2];   // lat, lon (grados), la más vieja primero
    // calculado por tierra_preparar
    float m[9];                 // terrestre -> vista
    float solv[3];              // sol en coordenadas de vista
    float h[3];                 // vector medio sol-ojo (brillo del mar)
    // la tabla (la reserva quien llama, en PSRAM). tabla_rehacer = 1 cuando cambió la inclinación o el tamaño
    tierra_px_t *tabla;
    int tabla_rehacer;
    float solq[3];              // el sol sin el giro de longitud
    int giro16;                 // giro de longitud en dieciseisavos de téxel
    // zona que se dibuja (lo de afuera queda negro): con zoom sólo vale la pena lo que muestra la pantalla
    int clip_x0, clip_x1, clip_y0, clip_y1;
    int filtrar;                // 1 = filtrado bilineal de la textura (cuando el zoom la estira)
} tierra_t;

// dirección al sol para un instante UTC (segundos unix). Precisión ~0,5°: sobra para el terminador.
void tierra_sol(double unix_utc, float sol[3]);
// deja listas las matrices del cuadro (llamar una vez por cuadro, antes de repartir filas)
void tierra_preparar(tierra_t *e);
// dibuja las filas [y0, y1) de la imagen w x h RGB565 (se reparte entre los 2 núcleos). out apunta a la fila y0
// (así se puede dibujar en una baldosa chica en RAM interna y copiarla después con DMA)
void tierra_filas(const tierra_t *e, uint16_t *out, int w, int h, int y0, int y1);
// encima: sismos latiendo y la EEI con su estela (un solo núcleo, es poco)
void tierra_marcas(const tierra_t *e, uint16_t *out, int w, int h);
// lat/lon (grados) + altura relativa (0 = superficie) -> píxel; devuelve 1 si se ve (no queda detrás del globo)
int tierra_proyectar(const tierra_t *e, float lat, float lon, float alt, float *sx, float *sy);
// píxel -> lat/lon (grados); 0 si el píxel no cae sobre el globo
int tierra_desproyectar(const tierra_t *e, float sx, float sy, float *lat, float *lon);

#ifdef __cplusplus
}
#endif
