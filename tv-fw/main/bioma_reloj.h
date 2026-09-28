// BIOMA — reloj futurista para usarlo de fondo de pantalla (C puro: también corre en el PC para verlo antes).
// Formato de 12 horas: "4:42" grande, AM/PM y los segundos al lado, y un segundero que se llena durante el minuto.
// Se dibuja sobre la imagen chica de 480x270 (antes del escalado ×4 del PPA), así que sale en la webcam, en el
// video de la página y en las fotos. El juez de la evolución no lo ve: mira el rastro, no esta imagen.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void reloj_preparar(int hh24, int mm, int ss);          // arma las máscaras con brillo (sólo lo que cambió)
// lo suma a la imagen 480x270; dos_puntos parpadea cada segundo; ms_minuto (0..59999) mueve el segundero
void reloj_dibujar(uint16_t *img565, uint16_t brillo565, int dos_puntos, int ms_minuto);

#ifdef __cplusplus
}
#endif
