#include "sx1262.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

#define SX1262_SPI_HOST        SPI2_HOST

#define CMD_SET_STANDBY        0x80
#define CMD_SET_RX             0x82
#define CMD_SET_TX             0x83
#define CMD_SET_PACKET_TYPE    0x8A
#define CMD_SET_RF_FREQUENCY   0x86
#define CMD_SET_PA_CONFIG      0x95
#define CMD_SET_TX_PARAMS      0x8E
#define CMD_SET_MODULATION     0x8B
#define CMD_SET_PACKET_PARAMS  0x8C
#define CMD_SET_DIO_IRQ_PARAMS 0x08
#define CMD_GET_IRQ_STATUS     0x12
#define CMD_CLEAR_IRQ_STATUS   0x02
#define CMD_GET_RX_STATUS      0x13
#define CMD_READ_BUFFER        0x1E
#define CMD_WRITE_BUFFER       0x0E
#define CMD_SET_BUFFER_BASE    0x8F
#define CMD_SET_REGULATOR      0x96
#define CMD_SET_TCXO           0x97
#define CMD_CALIBRATE          0x89
#define CMD_CALIBRATE_IMAGE    0x98
#define CMD_SET_DIO2_AS_RF_SWITCH 0x9D

static const char *TAG = NODO_TAG;
static spi_device_handle_t radio;

static esp_err_t wait_busy(void)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (!gpio_get_level(PIN_BUSY)) return ESP_OK;
        esp_rom_delay_us(10);
    }
    return ESP_ERR_TIMEOUT;
}

/* Comandos SX1262 por SPI. El primer byte de una lectura es el status. */
static esp_err_t sx1262_command(uint8_t opcode, const uint8_t *params, size_t params_len,
                                 uint8_t *response, size_t response_len)
{
    uint8_t tx[260] = { opcode }, rx[260] = { 0 };
    size_t total = 1 + params_len + response_len;
    if (total > sizeof(tx)) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "radio ocupada");
    if (params_len) memcpy(tx + 1, params, params_len);
    spi_transaction_t t = {.length = total * 8, .tx_buffer = tx, .rx_buffer = rx};
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(radio, &t), TAG, "SPI");
    if (response_len) memcpy(response, rx + 1 + params_len, response_len);
    return ESP_OK;
}

static esp_err_t set_irq_routes(uint16_t dio1_mask)
{
    /* irqMask = 0xFFFF garantiza que todos los eventos (TxDone, RxDone, Timeout)
     * se registren siempre en GetIrqStatus, independientemente de que se mapeen
     * o no a un pin fisico DIO. */
    uint8_t params[] = {
        0xFF, 0xFF,
        (uint8_t)(dio1_mask >> 8), (uint8_t)dio1_mask,
        0, 0, 0, 0
    };
    return sx1262_command(CMD_SET_DIO_IRQ_PARAMS, params, sizeof(params), NULL, 0);
}

static esp_err_t sx1262_read_buffer(uint8_t offset, uint8_t *data, size_t length)
{
    uint8_t tx[3 + MAX_PACKET_LENGTH] = { CMD_READ_BUFFER, offset, 0 };
    uint8_t rx[3 + MAX_PACKET_LENGTH] = { 0 };
    if (length > MAX_PACKET_LENGTH) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "radio ocupada");
    spi_transaction_t t = {.length = (3 + length) * 8, .tx_buffer = tx, .rx_buffer = rx};
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(radio, &t), TAG, "SPI lectura");
    memcpy(data, rx + 3, length);
    return ESP_OK;
}

static esp_err_t sx1262_configure(void)
{
    const uint8_t dc_dc[] = { 1 }, tcxo[] = { 2, 0, 1, 0x40 }; /* 1.8 V, 5 ms */
    const uint8_t calibration[] = { 0x7F }, image[] = { 0xE1, 0xE9 }, standby[] = { 0 };
    const uint8_t lora[] = { 1 }, pa[] = { 4, 7, 0, 1 }, tx_power[] = { 14, 4 };
    const uint8_t modulation[] = { 7, 4, 1, 0 }; /* SF7, 125 kHz, CR 4/5 */
    const uint8_t packet[] = { 0, 8, 0, 0xFF, 1, 0 }; /* CRC, encabezado explicito */
    const uint8_t buffer[] = { 0, 0 };
    const uint8_t dio2_rf[] = { 1 };
    uint32_t rf = (uint32_t)(((uint64_t)FREQ_HZ << 25) / 32000000UL);
    uint8_t frequency[] = { rf >> 24, rf >> 16, rf >> 8, rf };

    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_REGULATOR, dc_dc, sizeof(dc_dc), NULL, 0), TAG, "DC-DC");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_TCXO, tcxo, sizeof(tcxo), NULL, 0), TAG, "TCXO");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_DIO2_AS_RF_SWITCH, dio2_rf, sizeof(dio2_rf), NULL, 0), TAG, "DIO2 RF switch");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CALIBRATE, calibration, sizeof(calibration), NULL, 0), TAG, "calibracion");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CALIBRATE_IMAGE, image, sizeof(image), NULL, 0), TAG, "calibracion imagen");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_STANDBY, standby, sizeof(standby), NULL, 0), TAG, "standby");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_PACKET_TYPE, lora, sizeof(lora), NULL, 0), TAG, "tipo LoRa");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_RF_FREQUENCY, frequency, sizeof(frequency), NULL, 0), TAG, "frecuencia");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_PA_CONFIG, pa, sizeof(pa), NULL, 0), TAG, "PA");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_TX_PARAMS, tx_power, sizeof(tx_power), NULL, 0), TAG, "potencia");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_MODULATION, modulation, sizeof(modulation), NULL, 0), TAG, "modulacion");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_PACKET_PARAMS, packet, sizeof(packet), NULL, 0), TAG, "paquete");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_BUFFER_BASE, buffer, sizeof(buffer), NULL, 0), TAG, "buffer");
    return set_irq_routes(0x0262);
}

esp_err_t sx1262_init(void)
{
    ESP_RETURN_ON_ERROR(gpio_config(&(gpio_config_t){
        .pin_bit_mask = (1ULL << PIN_DIO1) | (1ULL << PIN_BUSY),
        .mode = GPIO_MODE_INPUT}), TAG, "GPIO entradas");
    ESP_RETURN_ON_ERROR(gpio_config(&(gpio_config_t){
        .pin_bit_mask = 1ULL << PIN_RESET,
        .mode = GPIO_MODE_OUTPUT}), TAG, "GPIO reset");

    gpio_set_level(PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(spi_bus_initialize(SX1262_SPI_HOST, &(spi_bus_config_t){
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .max_transfer_sz = 3 + MAX_PACKET_LENGTH}, SPI_DMA_CH_AUTO), TAG, "bus SPI");
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SX1262_SPI_HOST, &(spi_device_interface_config_t){
        .clock_speed_hz = LORA_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = PIN_NSS,
        .queue_size = 1}, &radio), TAG, "dispositivo SPI");

    return sx1262_configure();
}

esp_err_t sx1262_start_receive(void)
{
    const uint8_t clear[] = { 0xFF, 0xFF }, continuous[] = { 0xFF, 0xFF, 0xFF };
    /* sx1262_send_ack deja PayloadLength = largo del ACK (20). Se restaura el
     * maximo (0xFF) antes de volver a RX para no limitar la siguiente trama. */
    const uint8_t rx_packet[] = { 0, 8, 0, 0xFF, 1, 0 }; /* CRC, encabezado explicito */
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_PACKET_PARAMS, rx_packet, sizeof(rx_packet),
                                       NULL, 0), TAG, "parametros de recepcion");
    ESP_RETURN_ON_ERROR(set_irq_routes(0x0262), TAG, "configurar IRQ de recepcion");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CLEAR_IRQ_STATUS, clear, sizeof(clear), NULL, 0), TAG, "limpiar IRQ");
    return sx1262_command(CMD_SET_RX, continuous, sizeof(continuous), NULL, 0);
}

esp_err_t sx1262_send_ack(uint64_t message_id)
{
    char ack[24];
    int ack_length = snprintf(ack, sizeof(ack), "ACK:%016" PRIX64, message_id);
    if (ack_length <= 0 || (size_t)ack_length >= sizeof(ack))
        return ESP_ERR_INVALID_SIZE;

    const uint8_t standby[] = { 0 };
    const uint8_t clear[] = { 0xFF, 0xFF };
    uint8_t packet_params[] = { 0, 8, 0, (uint8_t)ack_length, 1, 0 };
    uint8_t buffer_params[sizeof(ack)];
    buffer_params[0] = 0;
    memcpy(buffer_params + 1, ack, (size_t)ack_length);
    const uint8_t tx_timeout[] = { 0x00, 0xC8, 0x00 };

    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_STANDBY, standby, sizeof(standby),
                                       NULL, 0), TAG, "preparar envio ACK");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_PACKET_PARAMS, packet_params,
                                       sizeof(packet_params), NULL, 0),
                        TAG, "configurar paquete ACK");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_WRITE_BUFFER, buffer_params,
                                       (size_t)ack_length + 1, NULL, 0),
                        TAG, "escribir ACK");
    ESP_RETURN_ON_ERROR(set_irq_routes(0x0201), TAG, "configurar IRQ de ACK");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CLEAR_IRQ_STATUS, clear,
                                       sizeof(clear), NULL, 0),
                        TAG, "limpiar IRQ antes de ACK");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_TX, tx_timeout,
                                       sizeof(tx_timeout), NULL, 0),
                        TAG, "transmitir ACK");

    /* Sondeo del registro IRQ (igual que ra01s.c del sensor).
     * Se usa esp_timer_get_time() porque con CONFIG_FREERTOS_HZ=100,
     * pdMS_TO_TICKS(5) es 0 y vTaskDelay(0) no espera. vTaskDelay(1)
     * cede 1 tick real (~10 ms). */
    int64_t start_us = esp_timer_get_time();
    uint8_t irq[3] = { 0 };
    uint16_t flags = 0;
    while ((esp_timer_get_time() - start_us) < 600000) {
        vTaskDelay(1);
        if (sx1262_command(CMD_GET_IRQ_STATUS, NULL, 0, irq, sizeof(irq)) == ESP_OK) {
            flags = ((uint16_t)irq[1] << 8) | irq[2];
            if (flags & 0x0001) break; /* TxDone */
            if (flags & 0x0200) break; /* Timeout */
        }
    }
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);

    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CLEAR_IRQ_STATUS, clear,
                                       sizeof(clear), NULL, 0),
                        TAG, "limpiar IRQ despues de ACK");
    if ((flags & 0x0001) == 0) {
        ESP_LOGW(TAG, "No se confirmo la transmision del ACK %016" PRIX64 " (flags=0x%04X, wait=%lu ms)",
                 message_id, flags, (unsigned long)elapsed_ms);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "ACK transmitido: ACK:%016" PRIX64 " (%lu ms)", message_id, (unsigned long)elapsed_ms);
    return ESP_OK;
}

esp_err_t sx1262_receive_packet(uint8_t *packet, size_t *length)
{
    uint8_t irq[3], status[3];
    const uint8_t clear[] = { 0xFF, 0xFF };
    if (!gpio_get_level(PIN_DIO1)) return ESP_ERR_NOT_FINISHED;
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_GET_IRQ_STATUS, NULL, 0, irq, sizeof(irq)), TAG, "leer IRQ");
    uint16_t flags = ((uint16_t)irq[1] << 8) | irq[2];
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CLEAR_IRQ_STATUS, clear, sizeof(clear), NULL, 0), TAG, "limpiar IRQ");
    if (!(flags & 0x0002) || (flags & 0x0060)) return ESP_FAIL; /* RX done, sin CRC/header error */
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_GET_RX_STATUS, NULL, 0, status, sizeof(status)), TAG, "estado RX");
    *length = status[1];
    if (!*length || *length >= MAX_PACKET_LENGTH) return ESP_ERR_INVALID_SIZE;
    return sx1262_read_buffer(status[2], packet, *length);
}
