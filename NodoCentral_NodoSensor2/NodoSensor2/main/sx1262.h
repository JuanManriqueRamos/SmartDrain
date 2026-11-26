#ifndef SX1262_H
#define SX1262_H

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#include "esp_err.h"

/*
 * Inicializa GPIO (DIO1, BUSY, RESET), reinicia el chip, configura el bus SPI,
 * agrega el dispositivo y aplica la configuracion LoRa (915 MHz, SF7, BW 125 kHz, CR 4/5).
 */
esp_err_t sx1262_init(void);

/* Limpia las IRQ y pone el radio en recepcion continua. */
esp_err_t sx1262_start_receive(void);

/*
 * Intenta leer un paquete recibido.
 *  - ESP_OK: paquete valido en `packet` con `*length` bytes (< MAX_PACKET_LENGTH).
 *  - ESP_ERR_NOT_FINISHED: aun no hay paquete (DIO1 en bajo).
 *  - Otro codigo: paquete descartado por error.
 */
esp_err_t sx1262_receive_packet(uint8_t *packet, size_t *length);

/* Transmite ACK:<message_id> y deja el radio listo para volver a RX desde el caller. */
esp_err_t sx1262_send_ack(uint64_t message_id);

#endif /* SX1262_H */
