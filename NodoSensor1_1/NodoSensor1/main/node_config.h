#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include <stdbool.h>

/*
 * Configuracion central del nodo.
 * Cambia aqui pines, red, radio y tiempos de funcionamiento.
 */

/* Identidad y zona horaria */
#define NODE_ID "NodoSensor1"
#define NODE_TIMEZONE "COT5"
#define NODE_MIN_VALID_EPOCH 1704067200

/* Wi-Fi y NTP */
#define NODE_WIFI_SSID "CONFIGURAR_WIFI_SSID"
#define NODE_WIFI_PASSWORD "CONFIGURAR_WIFI_PASSWORD"
#define NODE_NTP_SERVER "pool.ntp.org"
#define NODE_WIFI_CONNECT_TIMEOUT_MS 30000
#define NODE_SNTP_SYNC_TIMEOUT_MS 20000

/* RTC DS3231 (I2C) */
#define NODE_DS3231_I2C_PORT I2C_NUM_0
#define NODE_DS3231_SDA_PIN GPIO_NUM_33
#define NODE_DS3231_SCL_PIN GPIO_NUM_34
#define NODE_DS3231_I2C_ADDRESS 0x68
#define NODE_DS3231_I2C_FREQUENCY_HZ 100000
#define NODE_DS3231_I2C_TIMEOUT_MS 1000

/* Radio LoRa SX1262 */
#define NODE_LORA_FREQUENCY_HZ 915000000UL
#define NODE_LORA_TX_POWER_DBM 14
#define NODE_LORA_REGULATOR_VOLTAGE 1.8f
#define NODE_LORA_USE_TCXO true
#define NODE_LORA_SPREADING_FACTOR 7
#define NODE_LORA_BANDWIDTH 4
#define NODE_LORA_CODING_RATE 1
#define NODE_LORA_PREAMBLE_LENGTH 8
#define NODE_LORA_HEADER_TYPE 0
#define NODE_LORA_CRC_ENABLED true
#define NODE_LORA_INVERT_IQ false
#define NODE_LORA_SEND_INTERVAL_MS 5000
#define NODE_LORA_ACK_TIMEOUT_MS 3000
/* ACK por software en la misma radio: intentos totales por mensaje y pausa
 * aleatoria entre intentos (evita colisiones repetidas). */
#define NODE_LORA_MAX_ATTEMPTS 3
#define NODE_LORA_RETRY_BACKOFF_MIN_MS 100
#define NODE_LORA_RETRY_BACKOFF_MAX_MS 400

/* MicroSD (SPI) */
#define NODE_SD_SPI_HOST SPI3_HOST
#define NODE_SD_CS_PIN GPIO_NUM_4
#define NODE_SD_SCK_PIN GPIO_NUM_5
#define NODE_SD_MOSI_PIN GPIO_NUM_1
#define NODE_SD_MISO_PIN GPIO_NUM_2
/* Frecuencia conservadora para modulos SD conectados por cables (400 kHz es inmune a ruido/CRC). */
#define NODE_SD_MAX_FREQ_KHZ 400
#define NODE_SD_MAX_TRANSFER_SIZE 4000
#define NODE_SD_MOUNT_POINT "/sdcard"
#define NODE_SD_MAX_OPEN_FILES 5
#define NODE_SD_ALLOCATION_UNIT_SIZE (16 * 1024)
#define NODE_SD_QUEUE_LINE_MAX 512

/* Sensor ultrasonico */
#define NODE_ULTRASONIC_TRIGGER_PIN GPIO_NUM_7
#define NODE_ULTRASONIC_ECHO_PIN GPIO_NUM_6
#define NODE_ULTRASONIC_MAX_TIMEOUT_US 30000
#define NODE_ULTRASONIC_TRIGGER_PULSE_US 10
#define NODE_ULTRASONIC_SOUND_SPEED_CM_PER_US 0.0343f
#define NODE_ULTRASONIC_MIN_VALID_CM 5.0f
#define NODE_ULTRASONIC_MAX_VALID_CM 400.0f
#define NODE_ULTRASONIC_POLL_INTERVAL_MS 100
#define NODE_ULTRASONIC_NO_RESPONSE_LIMIT 3

/* ADC HX710B (DOUT/PD_SCK, no usa I2C ni SPI) */
#define NODE_HX710B_DOUT_PIN GPIO_NUM_41
#define NODE_HX710B_SCK_PIN GPIO_NUM_42
#define NODE_HX710B_READY_TIMEOUT_MS 150
#define NODE_HX710B_CLOCK_DELAY_US 1
/* 27 pulsos: entrada diferencial a 40 muestras por segundo. */
#define NODE_HX710B_TOTAL_CLOCK_PULSES 27

/*
 * Calibracion de presion: reemplaza ambos puntos con lecturas reales y activa
 * la conversion. Las presiones deben ser manometricas y estar en kPa.
 */
#define NODE_HX710B_CALIBRATION_ENABLED 0
#define NODE_HX710B_CAL_POINT1_RAW 0
#define NODE_HX710B_CAL_POINT1_KPA 0.785f
#define NODE_HX710B_CAL_POINT2_RAW 0
#define NODE_HX710B_CAL_POINT2_KPA 1.569f
#define NODE_HX710B_PRESSURE_MAX_KPA 40.0f

/* Dimensiones interiores de la caja; el caudal estimado es neto. */
#define NODE_TANK_LENGTH_CM 28.0f
#define NODE_TANK_WIDTH_CM 18.0f
#define NODE_TANK_HEIGHT_CM 20.0f
#define NODE_WATER_DENSITY_KG_M3 1000.0f

#define NODE_PACKET_BUFFER_SIZE 512

#endif
