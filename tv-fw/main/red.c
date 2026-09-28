// red.c — internet para TIERRA VIVA, por el camino que haya:
//   1) por el cable USB (HUSB): el PC corre tierra\puente_internet.py, que deja conexiones esperando en el puerto
//      5001 del P4; cuando el P4 necesita un sitio escribe "CONNECT host:443\n" y el PC abre esa conexión.
//      Todas las conexiones las abre el PC: Windows no pide firewall ni administrador (el mismo túnel de Cerebro).
//   2) por Wi-Fi (el C3 montado encima), con un socket normal.
// El HTTPS lo hace el P4 con mbedTLS, que usa los aceleradores de hardware (AES, SHA, ECC, RSA).
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/ssl.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "red.h"
#include "wifi.h"

static const char *TAG = "red";
#define PORT_TUNEL 5001

static QueueHandle_t s_pool;              // sockets que el PC dejó esperando
static volatile int64_t s_ultimo_pc;      // última vez que el PC dejó un socket
static volatile int s_via;                // 0 nada, 1 cable, 2 wifi (la última que funcionó)
static volatile int s_ok_n, s_err_n, s_tls_ms;
static volatile int64_t s_hora_ok_us;

static void tunel_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));                    // que la red USB alcance a levantarse
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT_TUNEL), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 4) != 0) {
        ESP_LOGE(TAG, "no se pudo escuchar en el %d", PORT_TUNEL);
        vTaskDelete(NULL);
    }
    while (1) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        s_ultimo_pc = esp_timer_get_time();
        if (xQueueSend(s_pool, &fd, 0) != pdTRUE) close(fd);      // ya hay suficientes esperando
    }
}

static int por_cable(const char *host, int port)
{
    if (esp_timer_get_time() - s_ultimo_pc > 60000000LL && uxQueueMessagesWaiting(s_pool) == 0) return -1;
    int64_t fin = esp_timer_get_time() + 4000000;
    while (esp_timer_get_time() < fin) {
        int fd;
        if (xQueueReceive(s_pool, &fd, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        struct timeval tv = { .tv_sec = 40 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        char line[160];
        int n = snprintf(line, sizeof(line), "CONNECT %s:%d\n", host, port);
        if (send(fd, line, n, 0) != n) { close(fd); continue; }     // ese socket estaba muerto: probar otro
        int k = 0;
        char c;
        while (k < (int)sizeof(line) - 1 && recv(fd, &c, 1, 0) == 1) { if (c == '\n') break; line[k++] = c; }
        line[k] = 0;
        if (strncmp(line, "OK", 2) == 0) return fd;
        ESP_LOGW(TAG, "el PC no pudo abrir %s: '%s'", host, line);
        close(fd);
        return -1;
    }
    return -1;
}

static int por_wifi(const char *host, int port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { .tv_sec = 40 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { close(fd); fd = -1; }
    freeaddrinfo(res);
    return fd;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int r = send(*(int *)ctx, buf, len, 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : -0x004E;
    return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int r = recv(*(int *)ctx, buf, len, 0);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_TIMEOUT : -0x004C;
    return r;
}

static time_t dias_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (time_t)era * 146097 + (time_t)doe - 719468;
}

// "Date: Mon, 28 Sep 2026 07:05:49 GMT" -> reloj del sistema (y a BIOMA), si no hay una hora más fresca
static void hora_de_cabecera(const char *resp)
{
    const char *p = strstr(resp, "\r\nDate: ");
    if (!p) p = strstr(resp, "\r\ndate: ");
    if (!p) return;
    char mon[4] = "";
    int d, y, hh, mm, ss;
    if (sscanf(p + 8, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return;
    static const char *meses = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *mp = strstr(meses, mon);
    if (!mp || y < 2025) return;
    time_t t = dias_civil(y, (unsigned)((mp - meses) / 3 + 1), (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
    bool primera = s_hora_ok_us == 0;
    if (!primera && esp_timer_get_time() - s_hora_ok_us < 600000000LL) return;   // basta cada 10 min
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    s_hora_ok_us = esp_timer_get_time();
    wifi_hora_a_bioma();
}

int red_https_get(const char *host, const char *path, char *out, int cap, int *cuerpo)
{
    int via = 1, fd = por_cable(host, 443);
    if (fd < 0) { via = 2; fd = por_wifi(host, 443); }
    if (fd < 0) { s_err_n++; return -1; }
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    int n = -1, ret;
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto fin;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    esp_crt_bundle_attach(&conf);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto fin;
    mbedtls_ssl_set_hostname(&ssl, host);
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
    int64_t t0 = esp_timer_get_time();
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGW(TAG, "TLS con %s falló: -0x%04X", host, -ret);
            goto fin;
        }
    }
    s_tls_ms = (int)((esp_timer_get_time() - t0) / 1000);
    char req[400];
    // HTTP/1.0: la respuesta llega entera y sin "chunked"
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (TierraViva-ESP32P4)\r\n"
                                        "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    for (int w = 0; w < rl;) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req + w, rl - w);
        if (ret <= 0) goto fin;
        w += ret;
    }
    n = 0;
    while (n < cap - 1) {
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)out + n, cap - 1 - n);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) break;
        n += ret;
    }
    out[n] = 0;
    mbedtls_ssl_close_notify(&ssl);
fin:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    close(fd);
    if (n <= 0) { s_err_n++; return -1; }
    const char *b = strstr(out, "\r\n\r\n");
    int st = 0;
    sscanf(out, "HTTP/%*s %d", &st);
    if (!b || st != 200) { ESP_LOGW(TAG, "%s%s respondió %d", host, path, st); s_err_n++; return -1; }
    hora_de_cabecera(out);
    s_via = via;
    s_ok_n++;
    *cuerpo = (int)(b + 4 - out);
    return n;
}

int red_via(void) { return s_via; }

void red_estado_json(char *b, size_t cap)
{
    int64_t hace = s_ultimo_pc ? (esp_timer_get_time() - s_ultimo_pc) / 1000000 : -1;
    snprintf(b, cap, "{\"via\":\"%s\",\"ok\":%d,\"errores\":%d,\"tls_ms\":%d,\"pc_hace_s\":%lld,\"esperando\":%u}",
             s_via == 1 ? "cable USB" : s_via == 2 ? "wifi" : "ninguna", s_ok_n, s_err_n, s_tls_ms, (long long)hace,
             (unsigned)(s_pool ? uxQueueMessagesWaiting(s_pool) : 0));
}

void red_start(void)
{
    setenv("TZ", "<-04>4<-03>,M9.1.6/24,M4.1.6/24", 1);   // Chile continental
    tzset();
    s_pool = xQueueCreate(6, sizeof(int));
    xTaskCreatePinnedToCoreWithCaps(tunel_task, "tunel", 3072, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM);
}
