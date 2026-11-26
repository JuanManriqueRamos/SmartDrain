#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lora.h"
#include "node_config.h"
#include "ra01s.h"

static const char *TAG = "LoRa";

bool lora_init(void)
{
    LoRaInit();
    if (LoRaBegin(NODE_LORA_FREQUENCY_HZ, NODE_LORA_TX_POWER_DBM,
                  NODE_LORA_REGULATOR_VOLTAGE, NODE_LORA_USE_TCXO) != 0) {
        ESP_LOGE(TAG, "No se pudo inicializar el SX1262");
        return false;
    }

    LoRaConfig(NODE_LORA_SPREADING_FACTOR, NODE_LORA_BANDWIDTH,
               NODE_LORA_CODING_RATE, NODE_LORA_PREAMBLE_LENGTH,
               NODE_LORA_HEADER_TYPE, NODE_LORA_CRC_ENABLED, NODE_LORA_INVERT_IQ);
    ESP_LOGI(TAG, "LoRa listo: 915 MHz, SF7, BW125, CR4/5");
    return true;
}

bool lora_send(const uint8_t *payload, size_t length)
{
    if (payload == NULL || length == 0 || length > 255) {
        ESP_LOGE(TAG, "Payload LoRa invalido (longitud: %u)", (unsigned)length);
        return false;
    }

    if (!LoRaSend((uint8_t *)payload, (int16_t)length, SX126x_TXMODE_SYNC)) {
        ESP_LOGE(TAG, "Fallo el envio LoRa");
        return false;
    }

    return true;
}

/* millis() equivalente en ESP-IDF: tiempo monotono desde el arranque, en ms. */
static inline int64_t lora_millis(void)
{
    return esp_timer_get_time() / 1000;
}

/*
 * Espera el ACK en la MISMA radio SX1262, sin pines extra:
 * LoRaSend(..., SX126x_TXMODE_SYNC) deja el chip en recepcion continua
 * (SetRx) al terminar de transmitir, asi que aqui solo se sondea el buffer
 * de RX hasta que llega "ACK:<message_id>" o vence el timeout.
 * El sondeo cede la CPU cada 10 ms (vTaskDelay), no es una espera activa.
 */
static bool lora_wait_for_ack(const char *expected_ack, size_t expected_length,
                              uint32_t timeout_ms)
{
    uint8_t response[32];
    const int64_t start_ms = lora_millis();

    while (lora_millis() - start_ms < (int64_t)timeout_ms) {
        uint8_t response_length = LoRaReceive(response, sizeof(response));
        /* Comparacion binaria por longitud: no depende de un '\0' final. */
        if (response_length >= expected_length &&
            memcmp(response, expected_ack, expected_length) == 0 &&
            (response_length == expected_length || response[expected_length] == '\0')) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool lora_send_wait_ack(const uint8_t *payload, size_t length,
                        uint64_t message_id, uint32_t timeout_ms)
{
    char expected_ack[24];
    int ack_length = snprintf(expected_ack, sizeof(expected_ack),
                              "ACK:%016" PRIX64, message_id);
    if (ack_length <= 0 || (size_t)ack_length >= sizeof(expected_ack)) return false;

    for (int attempt = 1; attempt <= NODE_LORA_MAX_ATTEMPTS; attempt++) {
        if (lora_send(payload, length)) {
            if (lora_wait_for_ack(expected_ack, (size_t)ack_length, timeout_ms)) {
                ESP_LOGI(TAG, "ACK recibido para mensaje %016" PRIX64 " (intento %d/%d)",
                         message_id, attempt, NODE_LORA_MAX_ATTEMPTS);
                return true;
            }
            ESP_LOGW(TAG, "Sin ACK para mensaje %016" PRIX64 " en %" PRIu32
                     " ms (intento %d/%d)", message_id, timeout_ms,
                     attempt, NODE_LORA_MAX_ATTEMPTS);
        } else {
            ESP_LOGW(TAG, "Fallo TX del mensaje %016" PRIX64 " (intento %d/%d)",
                     message_id, attempt, NODE_LORA_MAX_ATTEMPTS);
        }

        if (attempt < NODE_LORA_MAX_ATTEMPTS) {
            /* Pausa aleatoria para no chocar de nuevo con otra transmision. */
            uint32_t backoff_ms = NODE_LORA_RETRY_BACKOFF_MIN_MS +
                (esp_random() % (NODE_LORA_RETRY_BACKOFF_MAX_MS -
                                 NODE_LORA_RETRY_BACKOFF_MIN_MS + 1));
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }
    }

    ESP_LOGE(TAG, "Mensaje %016" PRIX64 " sin ACK tras %d intentos; se continua",
             message_id, NODE_LORA_MAX_ATTEMPTS);
    return false;
}
