#ifndef DS3231_H
#define DS3231_H

#include <stdbool.h>
#include <time.h>

bool ds3231_init(void);
bool ds3231_get_time(time_t *value);
bool ds3231_set_time(time_t value);

#endif
