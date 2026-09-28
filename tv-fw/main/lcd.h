// BIOMA en la pantalla táctil de 2,8" (ILI9341 + XPT2046) enchufada en los 14 primeros pines del header derecho.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_http_server.h"

void lcd_start(void);
void lcd_cuadro_listo(const uint16_t *img);     // BIOMA: cuadro chico (480x270 RGB565) listo
void lcd_registrar(httpd_handle_t s);           // /api/pantalla (?brillo=0..100)
void lcd_estado_json(char *b, size_t cap);
