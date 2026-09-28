// Wi-Fi del P4 a través del ESP32-C3 montado encima (ESP-Hosted por UART).
#pragma once
#include <stddef.h>
#include "esp_http_server.h"

void wifi_start(void);                         // arranca sólo si hay una red guardada (y el reinicio anterior no fue una caída)
void wifi_registrar(httpd_handle_t s);         // /api/wifi
void wifi_estado_json(char *b, size_t cap);
void wifi_hora_a_bioma(void);                  // pasa la hora del sistema (ya puesta) a BIOMA y al núcleo LP
