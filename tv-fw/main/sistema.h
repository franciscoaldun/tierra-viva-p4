// Sistema común de los firmwares del laboratorio: guardián de vuelta atrás, OTA que protege a Cerebro P4,
// respaldo de la flash por la red y registro en memoria (ver sistema.c).
#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void sistema_early_init(void);             // PRIMERA línea de app_main
void sistema_registrar(httpd_handle_t s);  // /api/log /api/tasks /api/particiones /api/flash /api/volver /ota
void ota_mark_ok_if_pending(void);         // llamarlo cuando el PC habla con el firmware

#ifdef __cplusplus
}
#endif
