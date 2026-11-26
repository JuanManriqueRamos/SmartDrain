#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "ds3231.h"
#include "packet_parser.h"
#include "sensor_types.h"
#include "sx1262.h"
#include "sd_storage.h"
#include "wifi_app.h"

static const char *TAG = NODO_TAG;

/* ---- Deduplicacion de retransmisiones LoRa ----
 * Si el ACK se pierde en el aire, el sensor reenvia la MISMA trama (mismo
 * message_id). La Central responde ACK otra vez, pero no la vuelve a guardar. */
#define RECENT_IDS_CAPACITY 32
static uint64_t recent_ids[RECENT_IDS_CAPACITY];
static size_t recent_ids_count;
static size_t recent_ids_next;
static uint32_t duplicate_frames;

static bool recent_id_seen(uint64_t message_id)
{
    for (size_t i = 0; i < recent_ids_count; i++)
        if (recent_ids[i] == message_id) return true;
    return false;
}

static void recent_id_remember(uint64_t message_id)
{
    recent_ids[recent_ids_next] = message_id;
    recent_ids_next = (recent_ids_next + 1) % RECENT_IDS_CAPACITY;
    if (recent_ids_count < RECENT_IDS_CAPACITY) recent_ids_count++;
}

/* La trama del sensor es JSON ASCII imprimible: cualquier otro byte (0x00,
 * control, >0x7E) indica corrupcion y truncaria o alteraria el reenvio. */
static bool frame_is_printable_ascii(const uint8_t *frame, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (frame[i] < 0x20 || frame[i] > 0x7E) return false;
    return true;
}

static void send_ack_logged(uint64_t message_id)
{
    esp_err_t ack_err = sx1262_send_ack(message_id);
    if (ack_err != ESP_OK)
        ESP_LOGW(TAG, "No se pudo enviar ACK (%s)", esp_err_to_name(ack_err));
}

static bool set_system_time_from_rtc(void)
{
    time_t rtc_time;
    if (!ds3231_get_time(&rtc_time)) return false;
    struct timeval system_time = {.tv_sec = rtc_time, .tv_usec = 0};
    return settimeofday(&system_time, NULL) == 0;
}

static void add_rtc_timestamp(sensor_packet_t *sensor)
{
    if (sensor->timestamp[0] != '\0') return;
    time_t now = time(NULL);
    struct tm local_time;
    if (now >= NODO_MIN_VALID_EPOCH && localtime_r(&now, &local_time) != NULL &&
        strftime(sensor->timestamp, sizeof(sensor->timestamp),
                 "%Y-%m-%d %H:%M:%S", &local_time) > 0) {
        return;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando receptor SX1262 con ESP-IDF");
    setenv("TZ", NODO_TIMEZONE, 1);
    tzset();
    bool rtc_ready = ds3231_init();
    if (rtc_ready && set_system_time_from_rtc())
        ESP_LOGI(TAG, "Hora del sistema inicializada desde DS3231");
    else
        ESP_LOGW(TAG, "DS3231 no disponible o sin hora valida");
    bool sd_ready = sd_storage_init();
    int64_t last_sd_retry_us = esp_timer_get_time();
    if (!sd_ready) ESP_LOGE(TAG, "MicroSD no disponible; no se aceptaran lecturas para evitar perdidas silenciosas");
    wifi_app_connect();
    /* La tarea HTTP debe existir aun si la SD tarda en arrancar. De este modo
     * el reenvio al Cloud comienza automaticamente cuando un reintento de
     * montaje tenga exito, sin necesidad de reiniciar la Central. */
    ESP_ERROR_CHECK(wifi_app_start_upload(sd_ready));
    ESP_ERROR_CHECK(sx1262_init());
    ESP_ERROR_CHECK(sx1262_start_receive());
    ESP_LOGI(TAG, "LoRa listo: 915 MHz, SF7, BW 125 kHz, CR 4/5");
    while (true) {
        if (!sd_ready && esp_timer_get_time() - last_sd_retry_us >= 5000000) {
            last_sd_retry_us = esp_timer_get_time();
            ESP_LOGW(TAG, "Reintentando montar microSD...");
            sd_ready = sd_storage_init();
            if (sd_ready) {
                wifi_app_set_storage_ready(true);
                ESP_LOGI(TAG, "MicroSD montada; reenvio al Cloud habilitado");
            }
        }
        /* +1 reserva el terminador para packet_parse; LoRa puede entregar
         * exactamente MAX_PACKET_LENGTH bytes. */
        uint8_t raw[MAX_PACKET_LENGTH + 1] = { 0 }; size_t length = 0;
        esp_err_t err = sx1262_receive_packet(raw, &length);
        if (err == ESP_OK) {
            raw[length] = 0; sensor_packet_t sensor = { 0 };
            /* La trama es texto: un 0x00 interno truncaria el parseo en silencio. */
            bool printable = frame_is_printable_ascii(raw, length);
            bool parsed = printable && packet_parse((char *)raw, &sensor);
            if (!printable) {
                ESP_LOGW(TAG, "Trama con bytes no imprimibles (%u bytes); descartada",
                         (unsigned)length);
            } else if (!parsed) {
                ESP_LOGW(TAG, "Paquete invalido: %s", raw);
            } else if (recent_id_seen(sensor.message_id)) {
                /* Reintento del sensor: ya esta guardado, solo se repite el ACK. */
                duplicate_frames++;
                ESP_LOGW(TAG, "Trama %016" PRIX64 " repetida (ACK perdido); se reenvia ACK",
                         sensor.message_id);
                send_ack_logged(sensor.message_id);
            } else {
                add_rtc_timestamp(&sensor);
                if (sensor.timestamp[0] != '\0') {
                    ESP_LOGI(TAG, "Dato de %s: %s, distancia=%.2f cm, presion=%s",
                             sensor.node_id, sensor.timestamp, sensor.distance_cm,
                             sensor.pressure_valid ? "calculada" : "sin calibrar");
                } else {
                    ESP_LOGI(TAG, "Dato de %s sin hora valida, distancia=%.2f cm",
                             sensor.node_id, sensor.distance_cm);
                }
                /* Primero se guarda (microSD) y solo despues se responde ACK:
                 * un ACK garantiza que la trama no se pierde en la Central. */
                if (!wifi_app_queue_reading(&sensor, (const char *)raw, length,
                                            duplicate_frames)) {
                    ESP_LOGW(TAG, "No se pudo almacenar la lectura; sin ACK (el sensor reintentara)");
                } else {
                    recent_id_remember(sensor.message_id);
                    send_ack_logged(sensor.message_id);
                }
            }
            ESP_ERROR_CHECK(sx1262_start_receive());
        } else if (err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Paquete descartado: %s", esp_err_to_name(err));
            ESP_ERROR_CHECK(sx1262_start_receive());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
