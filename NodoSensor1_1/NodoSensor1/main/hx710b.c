#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hx710b.h"
#include "node_config.h"

#if NODE_HX710B_TOTAL_CLOCK_PULSES < 25 || NODE_HX710B_TOTAL_CLOCK_PULSES > 27
#error "NODE_HX710B_TOTAL_CLOCK_PULSES debe estar entre 25 y 27"
#endif

static const char *TAG = "HX710B";
static bool initialized;
static portMUX_TYPE hx710b_mux = portMUX_INITIALIZER_UNLOCKED;

static void clock_pulse(void)
{
    gpio_set_level(NODE_HX710B_SCK_PIN, 1);
    esp_rom_delay_us(NODE_HX710B_CLOCK_DELAY_US);
    gpio_set_level(NODE_HX710B_SCK_PIN, 0);
    esp_rom_delay_us(NODE_HX710B_CLOCK_DELAY_US);
}

bool hx710b_init(void)
{
    if (initialized) return true;

    gpio_config_t dout_config = {
        .pin_bit_mask = 1ULL << NODE_HX710B_DOUT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t sck_config = {
        .pin_bit_mask = 1ULL << NODE_HX710B_SCK_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&dout_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar DOUT (%s)", esp_err_to_name(err));
        return false;
    }
    err = gpio_config(&sck_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar PD_SCK (%s)", esp_err_to_name(err));
        return false;
    }
    err = gpio_set_level(NODE_HX710B_SCK_PIN, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo poner PD_SCK en bajo (%s)", esp_err_to_name(err));
        return false;
    }

    initialized = true;
    ESP_LOGI(TAG, "HX710B listo (DOUT=%d, SCK=%d)",
             NODE_HX710B_DOUT_PIN, NODE_HX710B_SCK_PIN);
    return true;
}

bool hx710b_read_raw(int32_t *value)
{
    if (!initialized || value == NULL) {
        ESP_LOGE(TAG, "HX710B no inicializado o destino invalido");
        return false;
    }

    int64_t deadline = esp_timer_get_time() +
                       (int64_t)NODE_HX710B_READY_TIMEOUT_MS * 1000;
    while (gpio_get_level(NODE_HX710B_DOUT_PIN) != 0) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "HX710B no entrego una lectura a tiempo");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint32_t raw = 0;
    portENTER_CRITICAL(&hx710b_mux);
    for (int bit = 0; bit < 24; bit++) {
        gpio_set_level(NODE_HX710B_SCK_PIN, 1);
        esp_rom_delay_us(NODE_HX710B_CLOCK_DELAY_US);
        raw = (raw << 1) | (uint32_t)gpio_get_level(NODE_HX710B_DOUT_PIN);
        gpio_set_level(NODE_HX710B_SCK_PIN, 0);
        esp_rom_delay_us(NODE_HX710B_CLOCK_DELAY_US);
    }
    for (int pulse = 24; pulse < NODE_HX710B_TOTAL_CLOCK_PULSES; pulse++)
        clock_pulse();
    portEXIT_CRITICAL(&hx710b_mux);

    if ((raw & 0x00800000U) != 0) raw |= 0xFF000000U;
    *value = (int32_t)raw;
    return true;
}
