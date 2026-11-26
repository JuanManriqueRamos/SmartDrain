#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "ds3231.h"
#include "hx710b.h"
#include "lora.h"
#include "sd_storage.h"
#include "ultrasonic_sensor.h"
#include "wifi_time_sync.h"
#include "node_config.h"

static const char *TAG = "NodoSensor1";

/* Maximo payload LoRa (lora_send rechaza > 255 y el central exige < 256). */
#define NODE_LORA_MAX_FRAME_LEN 255

static bool format_local_timestamp(time_t value, char *buffer, size_t size);
static bool hx710b_raw_to_pressure_kpa(int32_t raw, float *pressure_kpa);

static bool hx710b_raw_to_pressure_kpa(int32_t raw, float *pressure_kpa)
{
    const double raw_span = (double)NODE_HX710B_CAL_POINT2_RAW -
                            (double)NODE_HX710B_CAL_POINT1_RAW;
    const double pressure_span = (double)NODE_HX710B_CAL_POINT2_KPA -
                                 (double)NODE_HX710B_CAL_POINT1_KPA;

    if (!NODE_HX710B_CALIBRATION_ENABLED || pressure_kpa == NULL ||
        raw_span == 0.0 || pressure_span == 0.0 ||
        NODE_HX710B_CAL_POINT1_KPA < 0.0f ||
        NODE_HX710B_CAL_POINT1_KPA > NODE_HX710B_PRESSURE_MAX_KPA ||
        NODE_HX710B_CAL_POINT2_KPA < 0.0f ||
        NODE_HX710B_CAL_POINT2_KPA > NODE_HX710B_PRESSURE_MAX_KPA) {
        return false;
    }

    double converted = NODE_HX710B_CAL_POINT1_KPA +
                       ((double)raw - NODE_HX710B_CAL_POINT1_RAW) *
                       pressure_span / raw_span;
    if (!isfinite(converted) || converted < 0.0 ||
        converted > NODE_HX710B_PRESSURE_MAX_KPA) {
        return false;
    }

    *pressure_kpa = (float)converted;
    return true;
}

static bool pressure_to_water_level_cm(float pressure_kpa, float *level_cm)
{
    if (level_cm == NULL || NODE_WATER_DENSITY_KG_M3 <= 0.0f) return false;

    float level = pressure_kpa * 100000.0f /
                  (NODE_WATER_DENSITY_KG_M3 * 9.80665f);
    if (!isfinite(level) || level < 0.0f || level > NODE_TANK_HEIGHT_CM)
        return false;

    *level_cm = level;
    return true;
}

static bool format_local_timestamp(time_t value, char *buffer, size_t size)
{
    struct tm local_time;
    return localtime_r(&value, &local_time) != NULL &&
           strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &local_time) > 0;
}

static void update_sensor_status(const ultrasonic_reading_t *reading,
                                 int *no_response_count)
{
    if (reading->distance_cm < 0.0f) {
        if (*no_response_count < NODE_ULTRASONIC_NO_RESPONSE_LIMIT) (*no_response_count)++;
        if (*no_response_count == NODE_ULTRASONIC_NO_RESPONSE_LIMIT)
            ESP_LOGW(TAG, "Sensor sin respuesta o fuera de rango");
        return;
    }

    if (*no_response_count >= NODE_ULTRASONIC_NO_RESPONSE_LIMIT)
        ESP_LOGI(TAG, "Sensor respondiendo nuevamente");
    *no_response_count = 0;
    ESP_LOGI(TAG, "ECHO=%" PRId64 " us, distancia=%.2f cm",
             reading->echo_pulse_us, reading->distance_cm);
}

static bool remove_existing_message_id(char *payload)
{
    static const char field_name[] = "\"message_id\":";
    char *field = strstr(payload, field_name);
    if (field == NULL) return true;

    char *value = field + sizeof(field_name) - 1;
    if (*value != '"') {
        ESP_LOGE(TAG, "Campo message_id legado con formato invalido; paquete no enviado");
        return false;
    }

    char *value_end = strchr(value + 1, '"');
    if (value_end == NULL) {
        ESP_LOGE(TAG, "Campo message_id legado incompleto; paquete no enviado");
        return false;
    }

    char *remove_start = field;
    char *remove_end = value_end + 1;
    if (*remove_end == ',') {
        remove_end++;
    } else if (field > payload && field[-1] == ',') {
        remove_start--;
    }

    memmove(remove_start, remove_end, strlen(remove_end) + 1);
    ESP_LOGD(TAG, "Se reemplazara el message_id del paquete por el ID de la cola");
    return true;
}

static bool add_message_id(const char *payload, uint64_t message_id,
                           char *output, size_t output_size)
{
    static const char node_prefix[] = "{\"node_id\":\"" NODE_ID "\",";
    static char normalized_payload[NODE_PACKET_BUFFER_SIZE];
    size_t payload_length = strlen(payload);
    if (payload_length >= sizeof(normalized_payload)) {
        ESP_LOGE(TAG, "Paquete pendiente demasiado largo");
        return false;
    }

    memcpy(normalized_payload, payload, payload_length + 1);
    if (!remove_existing_message_id(normalized_payload)) return false;
    if (strncmp(normalized_payload, node_prefix, sizeof(node_prefix) - 1) != 0) {
        ESP_LOGE(TAG, "Formato de paquete pendiente invalido");
        return false;
    }

    int written = snprintf(output, output_size,
        "%s\"message_id\":\"%016" PRIX64 "\",%s",
        node_prefix, message_id, normalized_payload + sizeof(node_prefix) - 1);
    if (written <= 0 || (size_t)written >= output_size) {
        ESP_LOGE(TAG, "El paquete con message_id excede el buffer");
        return false;
    }
    return true;
}

static void transmit_pending_packets(void)
{
    uint64_t message_id;
    static char payload[NODE_PACKET_BUFFER_SIZE];
    static char payload_with_id[NODE_PACKET_BUFFER_SIZE + 32];

    while (true) {
        sd_queue_result_t result = sd_storage_peek(&message_id, payload, sizeof(payload));
        if (result == SD_QUEUE_EMPTY) return;
        if (result == SD_QUEUE_ERROR) return;
        if (!add_message_id(payload, message_id, payload_with_id,
                            sizeof(payload_with_id))) return;

        if (!lora_send_wait_ack((const uint8_t *)payload_with_id,
                                strlen(payload_with_id),
                                message_id, NODE_LORA_ACK_TIMEOUT_MS)) {
            return;
        }
        if (!sd_storage_ack(message_id)) return;
        ESP_LOGI(TAG, "Mensaje %016" PRIX64 " retirado de la cola SD", message_id);
    }
}

static void create_and_queue_packet(float distance_cm, bool sensor_ok, bool sd_ready,
                                    bool hx710b_ready)
{
    static bool has_previous_level;
    static float previous_level_cm;
    static int64_t previous_level_time_us;

    time_t now = time(NULL);
    char timestamp[25];
    char timestamp_json[sizeof(timestamp) + 2];
    char hx710b_json[16];
    char pressure_json[24] = "null";
    char level_json[24] = "null";
    char flow_json[24] = "null";
    static char payload[NODE_PACKET_BUFFER_SIZE];

    if (now >= NODE_MIN_VALID_EPOCH &&
        format_local_timestamp(now, timestamp, sizeof(timestamp))) {
        snprintf(timestamp_json, sizeof(timestamp_json), "\"%s\"", timestamp);
    } else {
        snprintf(timestamp_json, sizeof(timestamp_json), "null");
        ESP_LOGW(TAG, "Reloj sin sincronizacion; paquete con timestamp null");
    }

    int32_t hx710b_raw;
    if (hx710b_ready && hx710b_read_raw(&hx710b_raw)) {
        snprintf(hx710b_json, sizeof(hx710b_json), "%" PRId32, hx710b_raw);
        ESP_LOGI(TAG, "HX710B=%" PRId32 " cuentas", hx710b_raw);

        float pressure_kpa;
        if (hx710b_raw_to_pressure_kpa(hx710b_raw, &pressure_kpa)) {
            snprintf(pressure_json, sizeof(pressure_json), "%.2f", pressure_kpa);

            float level_cm;
            if (pressure_to_water_level_cm(pressure_kpa, &level_cm)) {
                int64_t sample_time_us = esp_timer_get_time();
                snprintf(level_json, sizeof(level_json), "%.2f", level_cm);
                if (has_previous_level && sample_time_us > previous_level_time_us) {
                    float base_area_cm2 = NODE_TANK_LENGTH_CM * NODE_TANK_WIDTH_CM;
                    float volume_change_l =
                        base_area_cm2 * (level_cm - previous_level_cm) / 1000.0f;
                    float elapsed_seconds =
                        (float)(sample_time_us - previous_level_time_us) / 1000000.0f;
                    float net_flow_l_min = volume_change_l * 60.0f / elapsed_seconds;
                    snprintf(flow_json, sizeof(flow_json), "%.2f", net_flow_l_min);
                    ESP_LOGI(TAG, "Presion=%.2f kPa, nivel=%.2f cm, caudal neto=%.2f L/min",
                             pressure_kpa, level_cm, net_flow_l_min);
                } else {
                    ESP_LOGI(TAG, "Presion=%.2f kPa, nivel=%.2f cm; esperando otra muestra",
                             pressure_kpa, level_cm);
                }

                previous_level_cm = level_cm;
                previous_level_time_us = sample_time_us;
                has_previous_level = true;
            } else {
                has_previous_level = false;
                ESP_LOGW(TAG, "Presion fuera del rango de nivel de la caja (0-%.1f cm)",
                         NODE_TANK_HEIGHT_CM);
            }
        } else {
            has_previous_level = false;
            ESP_LOGW(TAG, "No hay calibracion valida o la presion esta fuera de rango");
        }
    } else {
        snprintf(hx710b_json, sizeof(hx710b_json), "null");
        has_previous_level = false;
    }

    /* Nunca serializar NaN/Inf: "%.2f" produciria "nan"/"inf" (JSON invalido). */
    bool frame_sensor_ok = sensor_ok && isfinite(distance_cm);
    float frame_distance_cm = frame_sensor_ok ? distance_cm : 0.0f;
    if (strstr(pressure_json, "nan") != NULL || strstr(pressure_json, "inf") != NULL)
        snprintf(pressure_json, sizeof(pressure_json), "null");
    if (strstr(level_json, "nan") != NULL || strstr(level_json, "inf") != NULL)
        snprintf(level_json, sizeof(level_json), "null");
    if (strstr(flow_json, "nan") != NULL || strstr(flow_json, "inf") != NULL)
        snprintf(flow_json, sizeof(flow_json), "null");

    uint64_t message_id = ((uint64_t)esp_random() << 32) | esp_random();
    /* "battery" (siempre null) se omite: la trama completa llegaba a ~264 bytes
     * y LoRa solo admite 255. El central lo trata como campo opcional. */
    int len = snprintf(payload, sizeof(payload),
        "{\"node_id\":\"" NODE_ID "\","
        "\"message_id\":\"%016" PRIX64 "\","
        "\"timestamp\":%s,"
        "\"type\":\"distance\",\"value\":%.2f,\"unit\":\"cm\","
        "\"status\":\"%s\",\"hx710b_raw\":%s,"
        "\"pressure_kpa\":%s,\"water_level_cm\":%s,\"net_flow_l_min\":%s}",
        message_id, timestamp_json, frame_distance_cm,
        frame_sensor_ok ? "ok" : "sensor_error",
        hx710b_json, pressure_json, level_json, flow_json);

    if (len <= 0 || (size_t)len >= sizeof(payload) ||
        (size_t)len > NODE_LORA_MAX_FRAME_LEN) {
        ESP_LOGE(TAG, "El mensaje JSON (%d bytes) excede el maximo LoRa (%d)",
                 len, NODE_LORA_MAX_FRAME_LEN);
        return;
    }

    if (sd_ready) {
        if (!sd_storage_enqueue(message_id, payload)) return;
        ESP_LOGI(TAG, "Mensaje %016" PRIX64 " guardado en la cola SD", message_id);
        transmit_pending_packets();
    } else if (lora_send_wait_ack((const uint8_t *)payload, (size_t)len,
                                  message_id, NODE_LORA_ACK_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "Paquete LoRa confirmado: %s", payload);
    } else {
        ESP_LOGE(TAG, "No hay SD disponible; paquete %016" PRIX64 " no pudo entregarse",
                 message_id);
    }
}

void app_main(void)
{
    int no_response_count = 0;
    int64_t last_send_us = 0;
    int64_t last_sd_retry_us = 0;
    bool sd_ready;
    bool rtc_ready;
    bool hx710b_ready;

#if !NODE_HX710B_CALIBRATION_ENABLED
    ESP_LOGW(TAG, "Calibracion HX710B desactivada; presion, nivel y caudal seran null");
#endif

    setenv("TZ", NODE_TIMEZONE, 1);
    tzset();

    rtc_ready = ds3231_init();
    if (rtc_ready) {
        time_t rtc_time;
        if (ds3231_get_time(&rtc_time)) {
            struct timeval system_time = {
                .tv_sec = rtc_time,
                .tv_usec = 0
            };
            if (settimeofday(&system_time, NULL) == 0) {
                char timestamp[25];
                if (format_local_timestamp(rtc_time, timestamp, sizeof(timestamp)))
                    ESP_LOGI(TAG, "Reloj inicializado desde DS3231: %s", timestamp);
            } else {
                ESP_LOGE(TAG, "No se pudo establecer la hora del sistema desde el DS3231");
            }
        } else {
            ESP_LOGW(TAG, "No se pudo leer una hora valida del DS3231");
        }
    } else {
        ESP_LOGW(TAG, "DS3231 no disponible; se intentara obtener la hora por Wi-Fi/NTP");
    }

    if (wifi_time_sync_init() && rtc_ready) {
        if (!ds3231_set_time(time(NULL)))
            ESP_LOGE(TAG, "NTP sincronizo la hora, pero no se pudo actualizar el DS3231");
    }

    ultrasonic_sensor_init();
    ESP_LOGI(TAG, "Sensor ultrasonico inicializado");

    hx710b_ready = hx710b_init();
    if (!hx710b_ready)
        ESP_LOGW(TAG, "HX710B no disponible; se enviara hx710b_raw=null");

    if (!lora_init()) {
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    sd_ready = sd_storage_init();
    if (!sd_ready)
        ESP_LOGE(TAG, "Nodo sin redundancia SD; los paquetes no confirmados se perderan");

    while (true) {
        if (!sd_ready && esp_timer_get_time() - last_sd_retry_us >= 5000000) {
            last_sd_retry_us = esp_timer_get_time();
            ESP_LOGW(TAG, "Reintentando montar microSD...");
            sd_ready = sd_storage_init();
            if (sd_ready) ESP_LOGI(TAG, "MicroSD montada; cola persistente habilitada");
        }
        ultrasonic_reading_t reading = ultrasonic_sensor_read();
        bool sensor_ok = reading.distance_cm >= 0.0f;
        update_sensor_status(&reading, &no_response_count);

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_send_us >= (int64_t)NODE_LORA_SEND_INTERVAL_MS * 1000) {
            float distance_cm = sensor_ok ? reading.distance_cm : 0.0f;
            create_and_queue_packet(distance_cm, sensor_ok, sd_ready, hx710b_ready);
            last_send_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(NODE_ULTRASONIC_POLL_INTERVAL_MS));
    }
}
