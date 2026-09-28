// BIOMA — fondo vivo en el ESP32-P4. Declaraciones compartidas.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4_IP_STR   "192.168.7.1"
#define PORT_WEB    80
#define PORT_STREAM 81
#define P4_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

// ---------- genoma y simulación: bioma_sim.h (C puro, también corre en el PC) ----------
#include "bioma_sim.h"
#include "sistema.h"

// ---------- USB (usb.c, copiado de Cerebro P4) ----------
void     usb_start(void);
bool     usb_mounted(void);
bool     uvc_streaming(void);
int      uvc_format(void);
void     uvc_size(int *w, int *h);
bool     uvc_ready(void);
bool     uvc_send(const uint8_t *buf, size_t len);
uint32_t uvc_frames_sent(void);

// ---------- BIOMA (bioma.c) ----------
void   bioma_early_init(void);     // primero: reserva el rastro en RAM interna
void   bioma_start(void);
size_t bioma_copy_jpeg(uint8_t *dst, size_t cap, uint32_t *seq_io, TickType_t wait);
size_t bioma_copy_jpeg_chico(uint8_t *dst, size_t cap, uint32_t *seq_io, TickType_t wait);   // 480x256 para Wi-Fi
void   bioma_status(char *json, size_t cap);
void   bioma_set_evo_mode(int m);
void   bioma_set_time(uint32_t sec_of_day);   // hora real para el reloj (la manda el PC)
int    bioma_escena(void);                      // 0 = BIOMA, 1 = TIERRA VIVA
void   bioma_set_escena(int e);
void   bioma_toque(int x, int y);               // dedo en la pantalla (celdas del rastro 480x270)
void   bioma_set_clock(bool on);               // mostrar u ocultar el reloj  // 0 = juez JPEG · 1 = azar (control) · 2 = congelado (control)

// ---------- estadísticas (stats.c) ----------
typedef enum { ENG_JENC = 0, ENG_PPA, ENG_USB, ENG_COUNT } eng_t;
void  stats_start(void);
void  stats_busy(eng_t e, int64_t t0_us, int64_t t1_us);
void  stats_net(size_t rx, size_t tx);
void  stats_frame(void);
float stats_eng_pct(eng_t e);
float stats_cpu_pct(int core);
float stats_fps(void);
float stats_temp(void);

// ---------- núcleo LP (lpcore.c): reloj del día ----------
void     lpcore_start(void);
uint32_t lpcore_day_phase(void);    // 0..65535 = medianoche..medianoche
void     lpcore_set_time(uint32_t sec_of_day);  // el PC le dice la hora real
void     lpcore_set_speed(uint32_t speed);      // 1 = día real · 144 = un día cada 10 minutos
uint32_t lpcore_speed(void);


// ---------- web (web.c) ----------
void web_start(void);

#ifdef __cplusplus
}
#endif
