#include "wifi_app.h"

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config.h"
#include "sd_storage.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = NODO_TAG;
static EventGroupHandle_t wifi_events;
static SemaphoreHandle_t storage_mutex;
static bool storage_ready;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

void wifi_app_connect(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    wifi_event_handler, NULL));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t config = { .sta = {
        .scan_method = WIFI_ALL_CHANNEL_SCAN,
        .failure_retry_cnt = 10,
    }};
    strlcpy((char *)config.sta.ssid, NODO_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, NODO_WIFI_PASSWORD, sizeof(config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Conectando a Wi-Fi: %s", NODO_WIFI_SSID);
}

/* Devuelve el codigo HTTP de la respuesta, o -1 si no hubo respuesta. */
static int upload_payload(const char *body)
{
    if (!(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT)) {
        return -1;
    }
    esp_http_client_config_t config = {.url = NODO_API_URL, .timeout_ms = HTTP_TIMEOUT_MS};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "No se pudo crear cliente HTTP"); return -1; }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    /* La API Flask autentica mediante Authorization: Bearer <API_TOKEN>. */
    char authorization[320];
    int auth_length = snprintf(authorization, sizeof(authorization),
                               "Bearer %s", NODO_API_TOKEN);
    if (auth_length < 0 || auth_length >= (int)sizeof(authorization)) {
        ESP_LOGE(TAG, "API token demasiado largo");
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Lectura aceptada por Nodo Cloud (HTTP %d)", status);
    } else {
        ESP_LOGW(TAG, "Error enviando a Nodo Cloud: %s, HTTP %d",
                 esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(client);
    return err == ESP_OK ? status : -1;
}

/* Rechazos definitivos del Cloud (formato/integridad): reintentar no sirve y
 * bloquearia toda la cola. Esos registros se archivan en la SD, no se borran. */
static bool is_permanent_rejection(int status)
{
    return status == 400 || status == 413 || status == 422;
}

static void upload_task(void *argument)
{
    (void)argument;
    while (true) {
        if (storage_ready && (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT)) {
            while (true) {
                uint64_t message_id;
                /* static: solo esta tarea lo usa; evita 1 KB en su pila. */
                static char payload[NODO_SD_QUEUE_LINE_MAX];
                xSemaphoreTake(storage_mutex, portMAX_DELAY);
                sd_queue_result_t result = sd_storage_peek(&message_id, payload, sizeof(payload));
                xSemaphoreGive(storage_mutex);
                if (result == SD_QUEUE_EMPTY) break;
                if (result == SD_QUEUE_ERROR) {
                    ESP_LOGW(TAG, "No se pudo leer la cola persistente");
                    break;
                }
                int status = upload_payload(payload);
                bool delivered = status >= 200 && status < 300;
                if (!delivered && !is_permanent_rejection(status)) break; /* reintento luego */
                xSemaphoreTake(storage_mutex, portMAX_DELAY);
                if (!delivered) {
                    ESP_LOGE(TAG, "Nodo Cloud rechazo %016" PRIX64 " (HTTP %d); se archiva en rejected.log",
                             message_id, status);
                    if (!sd_storage_archive_rejected(message_id, payload)) {
                        xSemaphoreGive(storage_mutex);
                        ESP_LOGE(TAG, "No se pudo archivar el registro rechazado");
                        break;
                    }
                }
                bool acknowledged = sd_storage_ack(message_id);
                xSemaphoreGive(storage_mutex);
                if (!acknowledged) {
                    ESP_LOGW(TAG, "La API recibio el dato, pero no se pudo confirmar en SD");
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

esp_err_t wifi_app_start_upload(bool sd_is_ready)
{
    storage_ready = sd_is_ready;
    storage_mutex = xSemaphoreCreateMutex();
    if (storage_mutex == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(upload_task, "upload_task", UPLOAD_TASK_STACK_SIZE, NULL,
                    UPLOAD_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_app_set_storage_ready(bool ready)
{
    storage_ready = ready;
    ESP_LOGI(TAG, "Cola microSD %s", ready ? "habilitada" : "no disponible");
}

/* CRC-32 IEEE 802.3 (el mismo que zlib.crc32 en Python). Permite al Cloud
 * comprobar que la trama llego byte a byte igual a como la recibio la Central. */
static uint32_t crc32_ieee(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

/* Copia src como contenido de una cadena JSON (escapa " y \). */
static bool json_escape(const char *src, size_t src_length, char *dst, size_t dst_size)
{
    size_t n = 0;
    for (size_t i = 0; i < src_length; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (n + 2 >= dst_size) return false;
            dst[n++] = '\\';
        } else if (n + 1 >= dst_size) {
            return false;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
    return true;
}

/*
 * Sobre que se guarda en la microSD y se envia tal cual al Nodo Cloud:
 * {"gateway_id":"...","message_id":"<16 hex>","frame_length":N,
 *  "frame_crc32":"<8 hex>","gateway_time":"AAAA-MM-DD HH:MM:SS"|null,
 *  "central_ack":true,"duplicates":D,"frame":"<trama original escapada>"}
 * "frame" es la trama del sensor sin modificar (todas sus claves y nulos);
 * el Cloud verifica frame_length y frame_crc32 antes de aceptarla.
 */
bool wifi_app_queue_reading(const sensor_packet_t *sensor, const char *frame,
                            size_t frame_length, uint32_t duplicates)
{
    if (!storage_ready || storage_mutex == NULL || sensor == NULL ||
        frame == NULL || frame_length == 0) return false;

    /* static: solo la tarea principal (receptor LoRa) llama a esta funcion. */
    static char escaped_frame[2 * MAX_PACKET_LENGTH + 1];
    static char body[NODO_SD_QUEUE_LINE_MAX];

    if (!json_escape(frame, frame_length, escaped_frame, sizeof(escaped_frame))) {
        ESP_LOGE(TAG, "No se pudo escapar la trama para el reenvio");
        return false;
    }

    char gateway_time_json[32] = "null";
    time_t now = time(NULL);
    struct tm local_time;
    char gateway_time[24];
    if (now >= NODO_MIN_VALID_EPOCH && localtime_r(&now, &local_time) != NULL &&
        strftime(gateway_time, sizeof(gateway_time), "%Y-%m-%d %H:%M:%S", &local_time) > 0)
        snprintf(gateway_time_json, sizeof(gateway_time_json), "\"%s\"", gateway_time);

    int length = snprintf(body, sizeof(body),
        "{\"gateway_id\":\"%s\",\"message_id\":\"%016" PRIX64 "\","
        "\"frame_length\":%u,\"frame_crc32\":\"%08" PRIX32 "\","
        "\"gateway_time\":%s,\"central_ack\":true,\"duplicates\":%" PRIu32 ","
        "\"frame\":\"%s\"}",
        NODO_TAG, sensor->message_id, (unsigned)frame_length,
        crc32_ieee((const uint8_t *)frame, frame_length),
        gateway_time_json, duplicates, escaped_frame);
    if (length < 0 || length >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "El sobre para Nodo Cloud excede %d bytes", (int)sizeof(body));
        return false;
    }

    /* Se usa el message_id del sensor como clave de la cola: el Cloud lo usa
     * para descartar duplicados si un reenvio HTTP se repite. */
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    bool stored = sd_storage_enqueue(sensor->message_id, body);
    xSemaphoreGive(storage_mutex);
    if (!stored) ESP_LOGW(TAG, "No se pudo guardar la lectura en la microSD");
    return stored;
}
