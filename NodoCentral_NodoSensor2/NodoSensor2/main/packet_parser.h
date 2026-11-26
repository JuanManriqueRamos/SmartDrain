#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#include <stdbool.h>

#include "sensor_types.h"

/*
 * Parsea y valida un paquete de texto recibido por LoRa.
 * Devuelve true si el formato, el timestamp y los rangos son validos.
 */
bool packet_parse(const char *text, sensor_packet_t *out);

#endif /* PACKET_PARSER_H */
