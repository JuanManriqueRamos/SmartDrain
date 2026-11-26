#include <stdint.h>
#include <time.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "ds3231.h"
#include "node_config.h"

#define DS3231_STATUS_REGISTER 0x0F
#define DS3231_OSF_BIT (1U << 7)

static const char *TAG = "DS3231";
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t rtc_device;

static bool read_registers(uint8_t start_register, uint8_t *buffer, size_t length)
{
    esp_err_t err = i2c_master_transmit_receive(
        rtc_device, &start_register, sizeof(start_register),
        buffer, length, NODE_DS3231_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error leyendo registro 0x%02X del DS3231 (%s)",
                 start_register, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool write_registers(const uint8_t *data, size_t length)
{
    esp_err_t err = i2c_master_transmit(
        rtc_device, data, length, NODE_DS3231_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error escribiendo en el DS3231 (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool bcd_to_decimal(uint8_t bcd, uint8_t *value)
{
    uint8_t high = (bcd >> 4) & 0x0F;
    uint8_t low = bcd & 0x0F;
    if (high > 9 || low > 9) return false;
    *value = (uint8_t)(high * 10 + low);
    return true;
}

static uint8_t decimal_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static uint8_t days_in_month(int year, int month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 2 &&
        ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return days[month - 1];
}

bool ds3231_init(void)
{
    if (rtc_device != NULL) return true;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = NODE_DS3231_I2C_PORT,
        .sda_io_num = NODE_DS3231_SDA_PIN,
        .scl_io_num = NODE_DS3231_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar I2C (%s)", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = NODE_DS3231_I2C_ADDRESS,
        .scl_speed_hz = NODE_DS3231_I2C_FREQUENCY_HZ
    };

    err = i2c_master_bus_add_device(i2c_bus, &device_config, &rtc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo agregar el DS3231 al bus I2C (%s)",
                 esp_err_to_name(err));
        esp_err_t delete_err = i2c_del_master_bus(i2c_bus);
        if (delete_err != ESP_OK)
            ESP_LOGE(TAG, "No se pudo liberar el bus I2C (%s)",
                     esp_err_to_name(delete_err));
        i2c_bus = NULL;
        return false;
    }

    ESP_LOGI(TAG, "DS3231 listo en I2C 0x%02X (SDA=%d, SCL=%d)",
             NODE_DS3231_I2C_ADDRESS, NODE_DS3231_SDA_PIN, NODE_DS3231_SCL_PIN);
    return true;
}

bool ds3231_get_time(time_t *value)
{
    if (rtc_device == NULL || value == NULL) {
        ESP_LOGE(TAG, "DS3231 no inicializado o parametro de hora invalido");
        return false;
    }

    uint8_t status;
    if (!read_registers(DS3231_STATUS_REGISTER, &status, sizeof(status))) return false;
    if ((status & DS3231_OSF_BIT) != 0) {
        ESP_LOGW(TAG, "El oscilador del DS3231 se detuvo; la hora no es confiable");
        return false;
    }

    uint8_t registers[7];
    if (!read_registers(0x00, registers, sizeof(registers))) return false;

    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t raw_hour = registers[2];
    uint8_t raw_month = registers[5];

    if (!bcd_to_decimal(registers[0] & 0x7F, &second) ||
        !bcd_to_decimal(registers[1] & 0x7F, &minute) ||
        !bcd_to_decimal(registers[3] & 0x3F, &day) ||
        !bcd_to_decimal(raw_month & 0x1F, &month) ||
        !bcd_to_decimal(registers[6], &year)) {
        ESP_LOGE(TAG, "El DS3231 devolvio valores BCD invalidos");
        return false;
    }

    if ((raw_hour & (1U << 6)) != 0) {
        uint8_t hour_12;
        if (!bcd_to_decimal(raw_hour & 0x1F, &hour_12) ||
            hour_12 < 1 || hour_12 > 12) {
            ESP_LOGE(TAG, "El DS3231 devolvio una hora de 12 horas invalida");
            return false;
        }
        hour = (uint8_t)(hour_12 % 12);
        if ((raw_hour & (1U << 5)) != 0) hour = (uint8_t)(hour + 12);
    } else if (!bcd_to_decimal(raw_hour & 0x3F, &hour)) {
        ESP_LOGE(TAG, "El DS3231 devolvio una hora invalida");
        return false;
    }

    int full_year = 2000 + year + ((raw_month & 0x80) != 0 ? 100 : 0);
    if (second > 59 || minute > 59 || hour > 23 ||
        month < 1 || month > 12 || day < 1 ||
        day > days_in_month(full_year, month)) {
        ESP_LOGE(TAG, "El DS3231 devolvio una fecha u hora fuera de rango");
        return false;
    }

    struct tm rtc_time = {
        .tm_sec = second,
        .tm_min = minute,
        .tm_hour = hour,
        .tm_mday = day,
        .tm_mon = month - 1,
        .tm_year = full_year - 1900,
        .tm_isdst = -1
    };
    time_t converted = mktime(&rtc_time);
    if (converted < NODE_MIN_VALID_EPOCH) {
        ESP_LOGW(TAG, "La fecha del DS3231 es anterior al 1 de enero de 2024");
        return false;
    }

    *value = converted;
    return true;
}

bool ds3231_set_time(time_t value)
{
    if (rtc_device == NULL) {
        ESP_LOGE(TAG, "DS3231 no inicializado");
        return false;
    }

    struct tm current_time;
    if (localtime_r(&value, &current_time) == NULL ||
        current_time.tm_year + 1900 < 2000 ||
        current_time.tm_year + 1900 > 2199) {
        ESP_LOGE(TAG, "La fecha no se puede representar en el DS3231");
        return false;
    }

    int year = current_time.tm_year + 1900;
    uint8_t month = (uint8_t)(current_time.tm_mon + 1);
    uint8_t registers[] = {
        0x00,
        decimal_to_bcd((uint8_t)current_time.tm_sec),
        decimal_to_bcd((uint8_t)current_time.tm_min),
        decimal_to_bcd((uint8_t)current_time.tm_hour),
        decimal_to_bcd((uint8_t)(current_time.tm_wday + 1)),
        decimal_to_bcd((uint8_t)current_time.tm_mday),
        (uint8_t)(decimal_to_bcd(month) | (year >= 2100 ? 0x80 : 0)),
        decimal_to_bcd((uint8_t)(year % 100))
    };

    if (!write_registers(registers, sizeof(registers))) return false;

    uint8_t status;
    if (!read_registers(DS3231_STATUS_REGISTER, &status, sizeof(status))) return false;
    uint8_t clear_osf[] = {
        DS3231_STATUS_REGISTER,
        (uint8_t)(status & ~DS3231_OSF_BIT)
    };
    if (!write_registers(clear_osf, sizeof(clear_osf))) return false;

    ESP_LOGI(TAG, "Hora del DS3231 actualizada");
    return true;
}
