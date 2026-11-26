#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_time_sync.h"
#include "node_config.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "WiFiTime";
static EventGroupHandle_t wifi_event_group;

static bool format_local_timestamp(time_t value, char *buffer, size_t size)
{
    struct tm local_time;
    return localtime_r(&value, &local_time) != NULL &&
           strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &local_time) > 0;
}

static void sntp_sync_callback(struct timeval *tv)
{
    char timestamp[25];
    if (format_local_timestamp(tv->tv_sec, timestamp, sizeof(timestamp))) {
        ESP_LOGI(TAG, "NTP sincronizo la hora: %s", timestamp);
    } else {
        ESP_LOGW(TAG, "NTP sincronizo, pero no se pudo formatear la hora");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
            ESP_LOGE(TAG, "No se pudo iniciar conexion Wi-Fi (%s)", esp_err_to_name(err));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi desconectado (motivo %d); intentando reconectar", event->reason);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
            ESP_LOGE(TAG, "No se pudo reintentar Wi-Fi (%s)", esp_err_to_name(err));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado; IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_time_sync_init(void)
{
    if (NODE_WIFI_SSID[0] == '\0' || NODE_WIFI_PASSWORD[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi no configurado; agrega credenciales locales para sincronizar la hora");
        return false;
    }
    if (strlen(NODE_WIFI_SSID) >= sizeof(((wifi_config_t *)0)->sta.ssid) ||
        strlen(NODE_WIFI_PASSWORD) >= sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "SSID o contrasena Wi-Fi excede el tamano permitido");
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar NVS para Wi-Fi (%s)", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "No se pudo inicializar la interfaz de red (%s)", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "No se pudo crear el loop de eventos (%s)", esp_err_to_name(err));
        return false;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "No se pudo crear la interfaz Wi-Fi STA");
        return false;
    }

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el grupo de eventos Wi-Fi");
        return false;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar Wi-Fi (%s)", esp_err_to_name(err));
        return false;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo registrar eventos Wi-Fi (%s)", esp_err_to_name(err));
        return false;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo registrar evento IP (%s)", esp_err_to_name(err));
        return false;
    }

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, NODE_WIFI_SSID, strlen(NODE_WIFI_SSID));
    memcpy(wifi_config.sta.password, NODE_WIFI_PASSWORD, strlen(NODE_WIFI_PASSWORD));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar Wi-Fi como estacion (%s)", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar la red Wi-Fi (%s)", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar Wi-Fi (%s)", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Conectando a la red Wi-Fi configurada");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(NODE_WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "No se obtuvo conexion Wi-Fi; los paquetes usaran timestamp null");
        return false;
    }

    esp_log_level_set("esp_netif_sntp", ESP_LOG_DEBUG);
    esp_log_level_set("sntp", ESP_LOG_DEBUG);
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NODE_NTP_SERVER);
    sntp_config.sync_cb = sntp_sync_callback;
    err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar sincronizacion NTP (%s)", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NODE_SNTP_SYNC_TIMEOUT_MS));
    if (err != ESP_OK) {
        unsigned int reachability = 0;
        esp_err_t reachability_err = esp_netif_sntp_reachability(0, &reachability);
        if (reachability_err == ESP_OK) {
            ESP_LOGW(TAG, "Alcance del servidor NTP " NODE_NTP_SERVER ": 0x%x",
                     reachability);
        } else {
            ESP_LOGW(TAG, "No se pudo consultar alcance NTP (%s)",
                     esp_err_to_name(reachability_err));
        }
        ESP_LOGW(TAG, "No se sincronizo la hora por NTP (%s); se seguira intentando",
                 esp_err_to_name(err));
        return false;
    }

    time_t now = time(NULL);
    char timestamp[25];
    if (now < NODE_MIN_VALID_EPOCH ||
        !format_local_timestamp(now, timestamp, sizeof(timestamp))) {
        ESP_LOGW(TAG, "NTP respondio, pero la hora aun no es valida");
        return false;
    }

    ESP_LOGI(TAG, "Hora sincronizada por Wi-Fi/NTP: %s", timestamp);
    return true;
}
