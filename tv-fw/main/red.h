// Internet para TIERRA VIVA: por el cable USB (túnel del PC, tierra\puente_internet.py) o por Wi-Fi (C3).
#pragma once
#include <stddef.h>

void red_start(void);
// GET https://host/path. Devuelve los bytes leídos (cabeceras + cuerpo, terminado en 0) o -1; *cuerpo = dónde empieza el cuerpo.
// Sólo acepta respuestas 200. De paso pone la hora del sistema con la cabecera Date.
int  red_https_get(const char *host, const char *path, char *out, int cap, int *cuerpo);
int  red_via(void);                              // 0 ninguna, 1 cable USB, 2 wifi (la última que funcionó)
void red_estado_json(char *b, size_t cap);
