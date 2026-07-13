#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float offset_c;
    float slope;
    uint16_t time_constant_s;
} sen54_temperature_compensation_t;

esp_err_t sen54_temperature_compensation_get(
    sen54_temperature_compensation_t *parameters);

#ifdef __cplusplus
}
#endif
