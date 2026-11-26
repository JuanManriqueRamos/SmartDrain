#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { SD_QUEUE_ERROR = -1, SD_QUEUE_EMPTY = 0, SD_QUEUE_PENDING = 1 } sd_queue_result_t;

bool sd_storage_init(void);
bool sd_storage_enqueue(uint64_t message_id, const char *payload);
sd_queue_result_t sd_storage_peek(uint64_t *message_id, char *payload, size_t payload_size);
bool sd_storage_ack(uint64_t message_id);
/* Archiva en rejected.log un registro rechazado definitivamente por el Cloud. */
bool sd_storage_archive_rejected(uint64_t message_id, const char *payload);

#endif
