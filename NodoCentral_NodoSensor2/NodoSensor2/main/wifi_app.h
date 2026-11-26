#ifndef WIFI_APP_H
#define WIFI_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sensor_types.h"

/* Inicializa NVS, red, eventos y Wi-Fi en modo STA, e inicia la conexion. */
void wifi_app_connect(void);

/* Inicia la tarea que vacia la cola persistente de la microSD. Puede
 * iniciarse antes de que la tarjeta este disponible; queda esperando hasta
 * que wifi_app_set_storage_ready(true) la habilite. */
esp_err_t wifi_app_start_upload(bool sd_ready);

/* Habilita/deshabilita el acceso a la cola despues de montar la microSD. */
void wifi_app_set_storage_ready(bool ready);

/* Guarda primero en la microSD un sobre JSON con la trama original del sensor
 * (frame/frame_length), su CRC32 y metadatos de la Central, listo para el Cloud.
 * duplicates = retransmisiones detectadas desde el arranque (ACK perdidos). */
bool wifi_app_queue_reading(const sensor_packet_t *sensor, const char *frame,
                            size_t frame_length, uint32_t duplicates);

#endif /* WIFI_APP_H */
