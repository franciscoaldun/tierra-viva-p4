// BIOMA en la tele antigua (cable amarillo), sin PC: salida de video compuesto NTSC y sonido.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TV_PIN_DAC0   16      // IO16..IO21: el "DAC" de resistencias (IO16 = el bit menos pesado)
#define TV_PIN_AUDIO  22      // IO22: sonido PDM → filtro RC → RCA blanco
#define TV_PIN_PRUEBA 33      // IO33: reloj de píxel sólo durante la autoprueba (no conectar nada)
// IO28..IO31 + 5V/GND del final del header izquierdo quedan para el ESP32-C3 montado encima (Wi-Fi para el P4)

void tv_start(void);                                          // arranca la señal (con la configuración guardada)
void tv_cuadro_listo(const uint16_t *img, int ancho, int alto); // BIOMA: cuadro nuevo en img (480x270 RGB565)
void tv_esperar_libre(void);                                  // BIOMA: esperar que la tele termine de leer img
void tv_registrar(httpd_handle_t s);                          // /api/tv, /api/tv/prueba, /api/tv/captura.bin
void tv_evento_evolucion(bool aceptado);                      // una campanita cuando la evolución acepta un genoma

// sonido (tv_audio.c)
void tv_audio_start(void);
void tv_audio_set(bool on, int volumen);                      // volumen 0..100
void tv_audio_campana(void);
void tv_audio_dia(float fase);                                // 0..1 del día (noche grave, día brillante)

#ifdef __cplusplus
}
#endif
