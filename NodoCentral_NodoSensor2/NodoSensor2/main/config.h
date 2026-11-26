#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
/* Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262). */
#define NODO_TAG "NodoSensor2"

/* ---- Configuracion de red: edita estos valores antes de compilar. ---- */
#define NODO_WIFI_SSID      "CONFIGURAR_WIFI_SSID"
#define NODO_WIFI_PASSWORD  "CONFIGURAR_WIFI_PASSWORD"
#define NODO_API_URL        "http://192.0.2.1:5000/api/event"
#define NODO_API_TOKEN      "CONFIGURAR_API_TOKEN"

/* ---- Reloj DS3231 (I2C) ---- */
#define NODO_TIMEZONE "COT5"
#define NODO_MIN_VALID_EPOCH 1704067200
#define NODO_DS3231_I2C_PORT I2C_NUM_0
#define NODO_DS3231_SDA_PIN GPIO_NUM_33
#define NODO_DS3231_SCL_PIN GPIO_NUM_34
#define NODO_DS3231_I2C_ADDRESS 0x68
#define NODO_DS3231_I2C_FREQUENCY_HZ 100000
#define NODO_DS3231_I2C_TIMEOUT_MS 1000

/* ---- MicroSD (SPI3, independiente del bus del SX1262) ---- */
#define NODO_SD_SPI_HOST SPI3_HOST
#define NODO_SD_CS_PIN GPIO_NUM_4
#define NODO_SD_SCK_PIN GPIO_NUM_5
#define NODO_SD_MOSI_PIN GPIO_NUM_1
#define NODO_SD_MISO_PIN GPIO_NUM_2
/* 400 kHz es mas tolerante a cables Dupont/modulos sin adaptacion de nivel y previene errores de CRC.
 * Subelo solo despues de comprobar una comunicacion SD estable. */
#define NODO_SD_MAX_FREQ_KHZ 400
#define NODO_SD_MAX_TRANSFER_SIZE 4000
#define NODO_SD_MOUNT_POINT "/sdcard"
#define NODO_SD_MAX_OPEN_FILES 5
#define NODO_SD_ALLOCATION_UNIT_SIZE (16 * 1024)
/* Sobre JSON con la trama escapada (~550 B en el peor caso) + id + "|" */
#define NODO_SD_QUEUE_LINE_MAX 768

/* ---- Calibracion del transductor conectado al HX710B de NodoSensor1 ----
 * Ajusta estos valores con una lectura sin agua y otra con altura conocida.
 * PA_PER_COUNT puede ser negativo si el montaje invierte la señal.
 */
#define NODO_HX710B_CALIBRATED 0
#define NODO_HX710B_ZERO_RAW 0
#define NODO_HX710B_PA_PER_COUNT 1.0f
#define NODO_WATER_DENSITY_KG_M3 1000.0f
#define NODO_GRAVITY_M_S2 9.80665f

/* ---- Subida de datos (cola y tarea HTTP) ---- */
#define UPLOAD_QUEUE_LENGTH      16
#define UPLOAD_TASK_STACK_SIZE   6144
#define UPLOAD_TASK_PRIORITY     5
#define HTTP_TIMEOUT_MS          5000

/* ---- Radio LoRa ---- */
#define FREQ_HZ             915000000UL
#define MAX_PACKET_LENGTH   256
#define LORA_SPI_CLOCK_HZ   8000000

/* ---- Pines GPIO (SX1262 en Heltec WiFi LoRa 32 V3) ---- */
#define PIN_NSS    GPIO_NUM_8
#define PIN_DIO1   GPIO_NUM_14
#define PIN_RESET  GPIO_NUM_12
#define PIN_BUSY   GPIO_NUM_13
#define PIN_SCK    GPIO_NUM_9
#define PIN_MISO   GPIO_NUM_11
#define PIN_MOSI   GPIO_NUM_10

#endif /* CONFIG_H */
