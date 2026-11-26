#ifndef ULTRASONIC_SENSOR_H
#define ULTRASONIC_SENSOR_H

#include <stdint.h>

typedef struct {
    float distance_cm;
    int64_t echo_pulse_us;
} ultrasonic_reading_t;

void ultrasonic_sensor_init(void);
ultrasonic_reading_t ultrasonic_sensor_read(void);

#endif
