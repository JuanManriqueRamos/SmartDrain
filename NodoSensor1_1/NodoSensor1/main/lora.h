#ifndef LORA_H
#define LORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool lora_init(void);
bool lora_send(const uint8_t *payload, size_t length);
bool lora_send_wait_ack(const uint8_t *payload, size_t length,
                        uint64_t message_id, uint32_t timeout_ms);

#endif
