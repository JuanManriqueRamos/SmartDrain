#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "ultrasonic_sensor.h"
#include "node_config.h"

void ultrasonic_sensor_init(void)
{
    gpio_config_t trig = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << NODE_ULTRASONIC_TRIGGER_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .pull_up_en = GPIO_PULLUP_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&trig));

    gpio_config_t echo = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << NODE_ULTRASONIC_ECHO_PIN),
        .pull_down_en = GPIO_PULLDOWN_ENABLE, .pull_up_en = GPIO_PULLUP_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&echo));
    ESP_ERROR_CHECK(gpio_set_level(NODE_ULTRASONIC_TRIGGER_PIN, 0));
}

ultrasonic_reading_t ultrasonic_sensor_read(void)
{
    ultrasonic_reading_t reading = {
        .distance_cm = -1.0f,
        .echo_pulse_us = 0
    };

    gpio_set_level(NODE_ULTRASONIC_TRIGGER_PIN, 1);
    esp_rom_delay_us(NODE_ULTRASONIC_TRIGGER_PULSE_US);
    gpio_set_level(NODE_ULTRASONIC_TRIGGER_PIN, 0);

    int64_t wait_start = esp_timer_get_time();
    while (gpio_get_level(NODE_ULTRASONIC_ECHO_PIN) == 0) {
        if (esp_timer_get_time() - wait_start > NODE_ULTRASONIC_MAX_TIMEOUT_US) return reading;
    }

    int64_t echo_start = esp_timer_get_time();
    int64_t timeout = echo_start + NODE_ULTRASONIC_MAX_TIMEOUT_US;
    while (gpio_get_level(NODE_ULTRASONIC_ECHO_PIN) == 1) {
        if (esp_timer_get_time() >= timeout) return reading;
    }

    reading.echo_pulse_us = esp_timer_get_time() - echo_start;
    if (reading.echo_pulse_us <= 0) return reading;

    float distance = ((float)reading.echo_pulse_us *
                      NODE_ULTRASONIC_SOUND_SPEED_CM_PER_US) / 2.0f;
    if (distance < NODE_ULTRASONIC_MIN_VALID_CM || distance > NODE_ULTRASONIC_MAX_VALID_CM) return reading;

    reading.distance_cm = distance;
    return reading;
}
