// TIERRA VIVA en el P4 (texturas, datos vivos por internet, dedo y textos). El dibujo está en tierra.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tierra.h"

// lo que muestra la pantalla táctil en TIERRA VIVA: 320x240 1:1 (sin escalar) desde este punto de la imagen chica
#define TIERRA_LCD_X 80
#define TIERRA_LCD_Y 15

void tierra_start(void);
const tierra_t *tierra_preparar_cuadro(int w, int h);   // NULL si todavía no está lista
void tierra_dedo(bool apoyado, int x, int y);           // coordenadas de la imagen chica (480x270)
void tierra_textos(uint16_t *img, int w, int h);
void tierra_estado_json(char *buf, size_t cap);
