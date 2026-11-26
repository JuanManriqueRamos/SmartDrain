#ifndef SENSOR_TYPES_H
#define SENSOR_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t message_id;
    char node_id[16];
    char timestamp[25];
    float distance_cm;
    bool sensor_ok;
    bool hx710b_valid;
    int32_t hx710b_raw;
    bool pressure_valid;
    float pressure_pa;
    float water_level_cm;
} sensor_packet_t;

#endif /* SENSOR_TYPES_H */
