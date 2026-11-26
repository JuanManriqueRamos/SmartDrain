#include <stdint.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "config.h"
#include "ds3231.h"

#define DS3231_STATUS_REGISTER 0x0F
#define DS3231_OSF_BIT (1U << 7)

static const char *TAG = "DS3231";
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t rtc_device;

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
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[month - 1];
}

static bool read_registers(uint8_t start, uint8_t *data, size_t length)
{
    esp_err_t err = i2c_master_transmit_receive(rtc_device, &start, sizeof(start), data,
                                                  length, NODO_DS3231_I2C_TIMEOUT_MS);
    if (err == ESP_OK) return true;
    ESP_LOGE(TAG, "Error leyendo DS3231: %s", esp_err_to_name(err));
    return false;
}

static bool write_registers(const uint8_t *data, size_t length)
{
    esp_err_t err = i2c_master_transmit(rtc_device, data, length, NODO_DS3231_I2C_TIMEOUT_MS);
    if (err == ESP_OK) return true;
    ESP_LOGE(TAG, "Error escribiendo DS3231: %s", esp_err_to_name(err));
    return false;
}

bool ds3231_init(void)
{
    if (rtc_device != NULL) return true;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = NODO_DS3231_I2C_PORT,
        .sda_io_num = NODO_DS3231_SDA_PIN,
        .scl_io_num = NODO_DS3231_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar I2C: %s", esp_err_to_name(err));
        return false;
    }
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = NODO_DS3231_I2C_ADDRESS,
        .scl_speed_hz = NODO_DS3231_I2C_FREQUENCY_HZ,
    };
    err = i2c_master_bus_add_device(i2c_bus, &device_config, &rtc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo agregar DS3231: %s", esp_err_to_name(err));
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
        return false;
    }
    ESP_LOGI(TAG, "DS3231 listo (SDA=%d, SCL=%d)", NODO_DS3231_SDA_PIN, NODO_DS3231_SCL_PIN);
    return true;
}

bool ds3231_get_time(time_t *value)
{
    if (rtc_device == NULL || value == NULL) return false;
    uint8_t status;
    if (!read_registers(DS3231_STATUS_REGISTER, &status, sizeof(status)) || (status & DS3231_OSF_BIT)) {
        ESP_LOGW(TAG, "Hora del DS3231 no confiable");
        return false;
    }
    uint8_t data[7], second, minute, hour, day, month, year;
    if (!read_registers(0, data, sizeof(data)) ||
        !bcd_to_decimal(data[0] & 0x7F, &second) || !bcd_to_decimal(data[1] & 0x7F, &minute) ||
        !bcd_to_decimal(data[3] & 0x3F, &day) || !bcd_to_decimal(data[5] & 0x1F, &month) ||
        !bcd_to_decimal(data[6], &year)) return false;
    if (data[2] & (1U << 6)) {
        uint8_t hour_12;
        if (!bcd_to_decimal(data[2] & 0x1F, &hour_12) || hour_12 < 1 || hour_12 > 12) return false;
        hour = (uint8_t)(hour_12 % 12 + ((data[2] & (1U << 5)) ? 12 : 0));
    } else if (!bcd_to_decimal(data[2] & 0x3F, &hour)) return false;
    int full_year = 2000 + year + ((data[5] & 0x80) ? 100 : 0);
    if (second > 59 || minute > 59 || hour > 23 || month < 1 || month > 12 ||
        day < 1 || day > days_in_month(full_year, month)) return false;
    struct tm rtc_time = {.tm_sec = second, .tm_min = minute, .tm_hour = hour,
                          .tm_mday = day, .tm_mon = month - 1, .tm_year = full_year - 1900,
                          .tm_isdst = -1};
    *value = mktime(&rtc_time);
    return *value >= NODO_MIN_VALID_EPOCH;
}

bool ds3231_set_time(time_t value)
{
    if (rtc_device == NULL) return false;
    struct tm current;
    if (localtime_r(&value, &current) == NULL) return false;
    int year = current.tm_year + 1900;
    if (year < 2000 || year > 2199) return false;
    uint8_t data[] = {0, decimal_to_bcd(current.tm_sec), decimal_to_bcd(current.tm_min),
                      decimal_to_bcd(current.tm_hour), decimal_to_bcd(current.tm_wday + 1),
                      decimal_to_bcd(current.tm_mday),
                      (uint8_t)(decimal_to_bcd(current.tm_mon + 1) | (year >= 2100 ? 0x80 : 0)),
                      decimal_to_bcd(year % 100)};
    if (!write_registers(data, sizeof(data))) return false;
    uint8_t status;
    if (!read_registers(DS3231_STATUS_REGISTER, &status, sizeof(status))) return false;
    uint8_t clear_osf[] = {DS3231_STATUS_REGISTER, (uint8_t)(status & ~DS3231_OSF_BIT)};
    return write_registers(clear_osf, sizeof(clear_osf));
}
