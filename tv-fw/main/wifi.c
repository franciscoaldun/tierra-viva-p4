// wifi.c — Wi-Fi para el P4 a través del ESP32-C3 montado encima del header (ESP-Hosted por UART a 3 Mbaud).
// El C3 corre el "esclavo" oficial de Espressif (ideas-p4/c3-wifi/slave) y el P4 usa la API esp_wifi de siempre
// (esp_wifi_remote la manda por el UART: P4 IO28 TX → C3 GPIO0, C3 GPIO1 → P4 IO29 RX, IO31 reinicia al C3).
//
// Qué da: la hora de internet (SNTP, hora de Chile con su cambio de horario) para el reloj de BIOMA sin el PC,
// y la página del P4 en la red de la casa. La red y la clave llegan desde la página (/api/wifi) y viven en la
// NVS (espacio "wifi"); nunca en el código ni en notas.
//
// Supervisor (el patrón del ejemplo host_hosted_events de Espressif): el C3 manda un latido cada 5 s. Si el latido
// se corta, si el C3 se reinicia (llega otro INIT) o si el enlace falla, se desarma todo (red, ESP-Hosted) y se vuelve
// a armar, reintentando hasta que el C3 conteste. El P4 NO se reinicia: BIOMA sigue corriendo.
//
// Cuidado con la placa: si el C3 no está o no responde, nada de esto debe botar al P4. El Wi-Fi sólo arranca si hay
// una red guardada, en su propia tarea, y no arranca solo si el reinicio anterior fue una caída.
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "app.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define LATIDO_SEG      5                 // el C3 manda un latido cada 5 s
#define SIN_LATIDO_US   (16 * 1000000LL)  // 3 latidos perdidos = el C3 ya no está
#define BIT_ARRIBA      BIT0              // el enlace con el C3 quedó arriba
#define BIT_REARMAR     BIT1              // pedido desde la página: reiniciar el C3 y volver a armar el Wi-Fi
#define BIT_C3_REINICIO BIT2              // el C3 volvió a arrancar: reenviarle la configuración
#define SIN_INTERNET_REINICIO_US (30 * 60 * 1000000LL)   // último recurso: 30 min sin IP → reinicio ordenado del P4

static char s_ssid[33];
static char s_clave[65];
// 0 sin red guardada · 1 conectando · 2 conectado · 3 buscando al C3 · 4 red/clave mal · 5 en pausa
static volatile int s_estado;
static volatile int s_reintentos;
static volatile bool s_hora_ok;
static volatile int64_t s_hora_us;
static char s_ip[20] = "";
static volatile int s_rssi;
static volatile int s_ultimo_motivo;
static volatile uint32_t s_latidos, s_rearmes;
static volatile int64_t s_ultimo_latido_us;
static volatile bool s_rearmando, s_primer_init, s_wifi_arriba;
static esp_netif_t *s_sta;
static TaskHandle_t s_task;
// guardián: el supervisor marca que está vivo en cada vuelta; si queda trabado en una llamada que nunca vuelve
// (medido: esperando al C3 después de montarlo en caliente), el guardián reinicia el P4 en orden. Tope de 5 seguidos.
static volatile int64_t s_super_vivo_us, s_ultimo_ok_us;
RTC_NOINIT_ATTR static uint32_t s_guardia_magia, s_guardia_reinicios;
static EventGroupHandle_t s_ev;

static const char *estados[] = { "sin red guardada", "conectando", "conectado", "buscando al C3 (reintento automático)",
                                 "la red o la clave no calzan",
                                 "en pausa: el reinicio anterior fue una caída (apreta Conectar para reintentar)" };

// copias de texto acotadas: nunca se pasan del espacio de destino (el compilador lo exige con -Werror)
static void copiar_cfg(uint8_t *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap);
    memset(dst, 0, cap);
    memcpy(dst, src, n);
}

static void copiar_str(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void poner_red(wifi_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    copiar_cfg(wc->sta.ssid, sizeof(wc->sta.ssid), s_ssid);
    copiar_cfg(wc->sta.password, sizeof(wc->sta.password), s_clave);
    wc->sta.threshold.authmode = s_clave[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
}

static void cargar(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_clave[0] = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) s_ssid[0] = 0;
    n = sizeof(s_clave);
    if (nvs_get_str(h, "clave", s_clave, &n) != ESP_OK) s_clave[0] = 0;
    nvs_close(h);
}

static void guardar(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    if (s_ssid[0]) { nvs_set_str(h, "ssid", s_ssid); nvs_set_str(h, "clave", s_clave); }
    else { nvs_erase_key(h, "ssid"); nvs_erase_key(h, "clave"); }     // sólo estas dos llaves; la NVS es compartida
    nvs_commit(h);
    nvs_close(h);
}

// ---------------- hora de internet ----------------
void wifi_hora_a_bioma(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    if (lt.tm_year < 125) return;                        // todavía 1970: no hay hora
    uint32_t seg = (uint32_t)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
    bioma_set_time(seg);
    lpcore_set_time(seg);
    s_hora_ok = true;
    s_hora_us = esp_timer_get_time();
    ESP_LOGI(TAG, "hora de internet: %02d:%02d:%02d (Chile)", lt.tm_hour, lt.tm_min, lt.tm_sec);
}

static void on_sntp(struct timeval *tv) { wifi_hora_a_bioma(); }

// ---------------- eventos ----------------
static void on_hosted(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ESP_HOSTED_EVENT_CP_INIT) {
        // el C3 arrancó (la primera vez, o porque lo reiniciamos, o porque se reinició solo por un contacto que saltó):
        // perdió toda su configuración de Wi-Fi, así que hay que volver a mandársela
        if (!s_primer_init) s_primer_init = true;
        else { ESP_LOGW(TAG, "el C3 volvió a arrancar: le reenvío la configuración del Wi-Fi"); xEventGroupSetBits(s_ev, BIT_C3_REINICIO); }
    } else if (id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        xEventGroupSetBits(s_ev, BIT_ARRIBA);
    } else if (id == ESP_HOSTED_EVENT_CP_HEARTBEAT) {
        s_latidos++;
        s_ultimo_latido_us = esp_timer_get_time();
    }
}

static void on_evento(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_estado = 1;                                     // conecta wifi_task, después de ponerle la MAC a la interfaz
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_ultimo_motivo = d ? d->reason : 0;
        s_ip[0] = 0;
        if (s_rearmando) return;                          // la desconexión la provocamos nosotros al rearmar
        bool clave_mal = s_ultimo_motivo == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || s_ultimo_motivo == WIFI_REASON_AUTH_FAIL ||
                         s_ultimo_motivo == WIFI_REASON_NO_AP_FOUND || s_ultimo_motivo == WIFI_REASON_HANDSHAKE_TIMEOUT;
        s_estado = (clave_mal && s_reintentos > 3) ? 4 : 1;
        s_reintentos++;
        if (s_task) xTaskNotifyGive(s_task);              // la tarea reintenta con calma (no en el evento)
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_estado = 2;
        s_reintentos = 0;
        ESP_LOGI(TAG, "conectado a la red de la casa: %s", s_ip);
        s_guardia_reinicios = 0;
        s_ultimo_ok_us = esp_timer_get_time();
        static bool sntp = false;
        if (!sntp) {
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            c.sync_cb = on_sntp;
            if (esp_netif_sntp_init(&c) == ESP_OK) sntp = true;
        }
    }
}

// ---------------- armar y desarmar la red sobre el C3 ----------------
static bool wifi_armar(void)
{
    if (!s_sta) s_sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t r = esp_wifi_init(&cfg);
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(r)); return false; }
    wifi_config_t wc;
    poner_red(&wc);
    esp_wifi_set_mode(WIFI_MODE_STA);
    // El 26-sep, tras varios reinicios bruscos del C3 (sin despedirse del hotspot), el iPhone dejó de contestarle la
    // autenticación (motivo 2) aunque lo escuchaba a -33 dBm. Con la dirección "administrada localmente" (bit 1 del primer
    // byte) el iPhone lo ve como un equipo nuevo. Es fija, así que el iPhone siempre ve el mismo equipo.
    uint8_t base[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, base) == ESP_OK && (base[0] | base[1] | base[2] | base[3] | base[4] | base[5])) {
        base[0] |= 0x02;
        if (esp_wifi_set_mac(WIFI_IF_STA, base) == ESP_OK)
            ESP_LOGI(TAG, "dirección nueva para el hotspot: %02x:%02x:%02x:%02x:%02x:%02x", base[0], base[1], base[2], base[3], base[4], base[5]);
        else
            ESP_LOGW(TAG, "no pude cambiar la dirección del C3");
    }
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    r = esp_wifi_start();
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(r)); return false; }
    // Medido: la interfaz del P4 quedaba con MAC 00:00:00:00:00:00, así que el router (el hotspot) nunca contestaba
    // el DHCP. La MAC de verdad es la de la radio del C3: se le pide y se le pone a la interfaz antes de conectar.
    uint8_t mac[6] = { 0 };
    for (int i = 0; i < 20; i++) {
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) {
        esp_netif_set_mac(s_sta, mac);
        ESP_LOGI(TAG, "MAC de la radio del C3: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGE(TAG, "el C3 no entregó su MAC");
    }
    // Sin ahorro de energía en la radio: con el hotspot del iPhone y el ahorro puesto se perdían 5 de cada 6 pings
    // (medido: en el P4 no se perdía nada). El P4 va enchufado; el C3 gasta ~80 mA más.
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) ESP_LOGW(TAG, "no pude apagar el ahorro de energía de la radio");
    // Prueba de la hipótesis "alimentación por contactos sin soldar": el C3 escucha el hotspot a -32 dBm pero la
    // autenticación nunca llega. Transmitir a 20 dBm pide picos de ~300 mA; a 11 dBm, bastante menos. Con el iPhone
    // tan cerca sobra señal. (unidad: 0,25 dBm)
    if (esp_wifi_set_max_tx_power(44) == ESP_OK) ESP_LOGI(TAG, "potencia de transmisión del C3: 11 dBm");
    else ESP_LOGW(TAG, "no pude bajar la potencia de transmisión");
    // diagnóstico: ¿el C3 escucha la red guardada y con qué fuerza?
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 12;
        wifi_ap_record_t *ap = calloc(n, sizeof(wifi_ap_record_t));
        if (ap && esp_wifi_scan_get_ap_records(&n, ap) == ESP_OK) {
            bool vista = false;
            for (int i = 0; i < n; i++) {
                if (!strcmp((const char *)ap[i].ssid, s_ssid)) {
                    vista = true;
                    s_rssi = ap[i].rssi;
                    ESP_LOGI(TAG, "escaneo: \"%s\" en canal %d con %d dBm (seguridad %d)", s_ssid, ap[i].primary, ap[i].rssi, ap[i].authmode);
                }
            }
            if (!vista) ESP_LOGW(TAG, "escaneo: %d redes a la vista, pero no \"%s\"", n, s_ssid);
        }
        free(ap);
    } else {
        ESP_LOGW(TAG, "no pude escanear");
    }
    s_wifi_arriba = true;
    s_estado = 1;
    s_reintentos = 0;
    esp_wifi_connect();
    return true;
}

static void wifi_desarmar(void)
{
    // como el ejemplo de Espressif: avisarle a la interfaz que se perdió la conexión y que la estación paró
    wifi_event_sta_disconnected_t ev = { .reason = WIFI_REASON_CONNECTION_FAIL };
    esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev, sizeof(ev), pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL, 0, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(100));
    if (s_sta) { esp_netif_destroy_default_wifi(s_sta); s_sta = NULL; }
    s_wifi_arriba = false;
    s_ip[0] = 0;
}

// ---------------- supervisor ----------------
// Medido el 26-sep: apagar el enlace (esp_hosted_deinit) se queda colgado para siempre en "Deinitializing UART bus".
// Por eso el enlace NUNCA se apaga: si algo falla, se reinicia sólo el C3 por su pin, y cuando vuelve a arrancar
// (avisa con su INIT) se le reenvía la configuración del Wi-Fi. El lector del P4 se resincroniza solo (ver el parche
// en components/espressif__esp_hosted/host/drivers/transport/uart/uart_drv.c).
static void pulso_reinicio_c3(void)
{
    // IO31 → GPIO3 del C3: abajo 5 ms y arriba = el C3 se reinicia (su manejador pide ≥ 0,5 ms abajo)
    ESP_LOGW(TAG, "reinicio el C3 por su pin (IO%d)", CONFIG_ESP_HOSTED_UART_GPIO_RESET_SLAVE);
    gpio_set_level(CONFIG_ESP_HOSTED_UART_GPIO_RESET_SLAVE, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(CONFIG_ESP_HOSTED_UART_GPIO_RESET_SLAVE, 1);
}

static uint32_t s_lat_armado;                              // latidos al armar: el corte cuenta sólo si este C3 ya latió

static void armar_todo(void)
{
    static int fallas_seguidas = 0;
    if (wifi_armar()) {
        // Latido desactivado: desde que se activó, el C3 no lograba autenticarse con el hotspot (motivo 2, AUTH_EXPIRE)
        // y sin él se conectaba al primer intento. Las fallas se detectan igual: consultas cada 10 s + aviso de arranque.
        fallas_seguidas = 0;
    } else if (++fallas_seguidas >= 3) {
        // el C3 arranca pero no acepta la configuración: el P4 se reinicia ordenado y todo parte de cero (auto-reparación)
        ESP_LOGE(TAG, "el C3 no acepta la configuración 3 veces seguidas: reinicio ordenado del P4");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    s_lat_armado = s_latidos;
    s_ultimo_latido_us = esp_timer_get_time();
}

static void rearme_suave(void)
{
    s_rearmando = true;
    if (s_wifi_arriba) wifi_desarmar();
    vTaskDelay(pdMS_TO_TICKS(300));
    s_rearmando = false;
    armar_todo();
    s_rearmes++;
}

static void wifi_task(void *arg)
{
    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_hosted, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evento, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evento, NULL);

    // 1) primer enlace: ESP-Hosted reinicia al C3 y espera su INIT; si no contesta, se insiste con pulsos cada 15 s
    s_estado = 3;
    ESP_LOGI(TAG, "hablando con el C3 por el UART (IO28/IO29, reinicio IO31)...");
    esp_hosted_connect_to_slave();
    s_super_vivo_us = esp_timer_get_time();
    while (!(xEventGroupWaitBits(s_ev, BIT_ARRIBA, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000)) & BIT_ARRIBA)) {
        s_super_vivo_us = esp_timer_get_time();
        ESP_LOGW(TAG, "el C3 no contestó (¿está bien montado?): insisto");
        pulso_reinicio_c3();
    }
    s_super_vivo_us = esp_timer_get_time();
    xEventGroupClearBits(s_ev, BIT_C3_REINICIO);           // el INIT de este arranque ya está considerado
    armar_todo();

    // 2) vigilar para siempre: latido (C3 → P4), consultas (P4 → C3), reinicios del C3, Wi-Fi, hora y último recurso
    int64_t ultima_hora = 0, ultima_consulta = esp_timer_get_time(), ultimo_pulso = 0;
    int fallas_consulta = 0;
    while (1) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        int64_t ahora = esp_timer_get_time();
        s_super_vivo_us = ahora;
        EventBits_t b = xEventGroupGetBits(s_ev);
        if (b & BIT_C3_REINICIO) {                        // volvió a arrancar: reenviarle todo
            xEventGroupClearBits(s_ev, BIT_C3_REINICIO);
            rearme_suave();
            fallas_consulta = 0;
            continue;
        }
        if (b & BIT_REARMAR) {                            // pedido desde la página
            xEventGroupClearBits(s_ev, BIT_REARMAR);
            pulso_reinicio_c3();                          // su INIT dispara el rearme
            ultimo_pulso = ahora;
            s_estado = 3;
            continue;
        }
        bool sin_latido = s_latidos != s_lat_armado && ahora - s_ultimo_latido_us > SIN_LATIDO_US;
        if (sin_latido && ahora - ultimo_pulso > 20 * 1000000LL) {
            ESP_LOGW(TAG, "el C3 dejó de latir hace %lld s", (long long)((ahora - s_ultimo_latido_us) / 1000000));
            s_estado = 3;
            pulso_reinicio_c3();
            ultimo_pulso = ahora;
            continue;
        }
        if (!sin_latido && ahora - ultima_consulta > 10 * 1000000LL) {   // ¿el C3 todavía entiende lo que le mando?
            ultima_consulta = ahora;
            wifi_mode_t modo;
            if (esp_wifi_get_mode(&modo) == ESP_OK) fallas_consulta = 0;
            else if (++fallas_consulta >= 3 && ahora - ultimo_pulso > 20 * 1000000LL) {
                ESP_LOGW(TAG, "el C3 no contesta consultas (3 seguidas)");
                pulso_reinicio_c3();
                ultimo_pulso = ahora;
                fallas_consulta = 0;
                continue;
            }
        }
        if (n && s_estado != 2 && s_wifi_arriba) {        // se cayó el Wi-Fi: reintento con espera creciente (2..30 s)
            int espera = 2 << (s_reintentos < 4 ? s_reintentos : 4);
            for (int i = 0; i < (espera > 30 ? 30 : espera) * 2 && !(xEventGroupGetBits(s_ev) & (BIT_C3_REINICIO | BIT_REARMAR)); i++)
                vTaskDelay(pdMS_TO_TICKS(500));
            if (s_estado != 2 && !(xEventGroupGetBits(s_ev) & (BIT_C3_REINICIO | BIT_REARMAR))) esp_wifi_connect();
        }
        if (s_estado == 2) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
            if (s_hora_ok && ahora - ultima_hora > 3600LL * 1000000) {       // reloj de BIOMA: 1 vez por hora
                wifi_hora_a_bioma();
                ultima_hora = ahora;
            }
        }
    }
}

static void reinicio_guardian(const char *por_que)
{
    s_guardia_reinicios++;
    ESP_LOGE(TAG, "guardián: %s → reinicio ordenado del P4 (%lu de 5 seguidos)", por_que, (unsigned long)s_guardia_reinicios);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void guardia_task(void *arg)
{
    if (s_guardia_magia != 0x57A11) { s_guardia_magia = 0x57A11; s_guardia_reinicios = 0; }   // tras un apagón
    s_ultimo_ok_us = esp_timer_get_time();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t ahora = esp_timer_get_time();
        if (s_guardia_reinicios >= 5) continue;           // ya lo intentó 5 veces seguidas: no insiste más
        if (s_super_vivo_us && ahora - s_super_vivo_us > 90 * 1000000LL)
            reinicio_guardian("el supervisor del Wi-Fi quedó trabado 90 s");
        if (s_primer_init && s_ssid[0] && s_estado != 2 && ahora - s_ultimo_ok_us > SIN_INTERNET_REINICIO_US)   // sólo si hay C3
            reinicio_guardian("30 minutos sin internet");
    }
}

static void arrancar_tarea(void)
{
    if (!s_ev) s_ev = xEventGroupCreate();
    if (!s_task) {
        xTaskCreatePinnedToCoreWithCaps(wifi_task, "wifi", 8192, NULL, 7, &s_task, 0, MALLOC_CAP_SPIRAM);   // pila en PSRAM: la RAM interna es de BIOMA
        xTaskCreatePinnedToCoreWithCaps(guardia_task, "wifi_guardia", 3072, NULL, 6, NULL, 1, MALLOC_CAP_SPIRAM);
    }
}

// Con un firmware recién subido (a prueba), el Wi-Fi NO arranca solo hasta que el PC lo confirma: si arrancarlo
// botara al P4 antes de confirmar, el cargador volvería al firmware anterior y no sabríamos por qué.
static void espera_y_arranca(void *arg)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    for (int i = 0; i < 600; i++) {
        if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    arrancar_tarea();
    vTaskDelete(NULL);
}

void wifi_start(void)
{
    setenv("TZ", "<-04>4<-03>,M9.1.6/24,M4.1.6/24", 1);    // Chile continental (America/Santiago)
    tzset();
    cargar();
    esp_reset_reason_t rr = esp_reset_reason();
    if (!s_ssid[0]) { s_estado = 0; ESP_LOGI(TAG, "sin red guardada: el Wi-Fi espera a que la pongas en la página"); return; }
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
        s_estado = 5;
        ESP_LOGW(TAG, "el reinicio anterior fue una caída: esta vez no arranco el Wi-Fi (se puede pedir desde la página)");
        return;
    }
    xTaskCreatePinnedToCore(espera_y_arranca, "wifi_esp", 3072, NULL, 3, NULL, 0);   // se borra sola: pila normal
}

void wifi_estado_json(char *b, size_t cap)
{
    int seg = s_hora_ok ? (int)((esp_timer_get_time() - s_hora_us) / 1000000) : -1;
    int lat = s_latidos ? (int)((esp_timer_get_time() - s_ultimo_latido_us) / 1000000) : -1;
    snprintf(b, cap, "{\"estado\":\"%s\",\"codigo\":%d,\"red\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"motivo\":%d,"
             "\"hora_internet\":%s,\"seg_desde_sync\":%d,\"latidos\":%lu,\"seg_desde_latido\":%d,\"rearmes\":%lu,"
             "\"reinicios_guardian\":%lu}",
             estados[s_estado], s_estado, s_ssid, s_ip, s_rssi, s_ultimo_motivo, s_hora_ok ? "true" : "false", seg,
             (unsigned long)s_latidos, lat, (unsigned long)s_rearmes, (unsigned long)s_guardia_reinicios);
}

static void decodificar(char *t)                          // %XX y + de encodeURIComponent
{
    char *w = t;
    for (char *p = t; *p; p++) {
        if (*p == '%' && p[1] && p[2]) { char h[3] = { p[1], p[2], 0 }; *w++ = (char)strtol(h, NULL, 16); p += 2; }
        else *w++ = (*p == '+') ? ' ' : *p;
    }
    *w = 0;
}

// /api/wifi?red=...&clave=...  guarda y conecta  ·  /api/wifi?conectar=1  ·  /api/wifi?olvidar=1  borra sólo la red
static esp_err_t h_wifi(httpd_req_t *r)
{
    char q[256];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        char red[100], clave[140], t[8];
        if (httpd_query_key_value(q, "red", red, sizeof(red)) == ESP_OK) {
            decodificar(red);
            copiar_str(s_ssid, sizeof(s_ssid), red);
            s_clave[0] = 0;
            if (httpd_query_key_value(q, "clave", clave, sizeof(clave)) == ESP_OK) {
                decodificar(clave);
                copiar_str(s_clave, sizeof(s_clave), clave);
            }
            guardar();
            ESP_LOGI(TAG, "red guardada: \"%s\" (la clave no se anota en el registro)", s_ssid);
            if (s_task && s_ev) xEventGroupSetBits(s_ev, BIT_REARMAR);    // rearma con la red nueva
            else arrancar_tarea();
        }
        if (httpd_query_key_value(q, "conectar", t, sizeof(t)) == ESP_OK && s_ssid[0]) {
            if (s_task && s_ev) xEventGroupSetBits(s_ev, BIT_REARMAR);
            else arrancar_tarea();
        }
        if (httpd_query_key_value(q, "olvidar", t, sizeof(t)) == ESP_OK) {
            s_ssid[0] = s_clave[0] = 0;
            guardar();
            if (s_wifi_arriba) esp_wifi_disconnect();
            s_estado = 0;
        }
    }
    char b[480];
    wifi_estado_json(b, sizeof(b));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, b);
}

void wifi_registrar(httpd_handle_t s)
{
    httpd_uri_t u = { .uri = "/api/wifi", .method = HTTP_GET, .handler = h_wifi };
    httpd_register_uri_handler(s, &u);
}
