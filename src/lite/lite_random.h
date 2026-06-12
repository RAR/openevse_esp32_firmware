#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void (*lite_random_backend_t)(uint8_t *buf, size_t len);
void lite_random_set_backend(lite_random_backend_t fn);  // test/integration injection
void lite_random_bytes(uint8_t *buf, size_t len);
