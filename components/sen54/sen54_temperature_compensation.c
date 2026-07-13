#include "sen54_temperature_compensation.h"
#include "sen54_i2c_bridge.h"
#include "sensirion/sen5x_i2c.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "sen54_tc";

esp_err_t sen54_temperature_compensation_get(
    sen54_temperature_compensation_t *parameters)
{
    if (!parameters) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = sen54_i2c_transaction_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transaction begin failed (%s)", esp_err_to_name(err));
        return err;
    }

    int16_t raw_offset = 0;
    int16_t raw_slope = 0;
    uint16_t raw_time_constant = 0;

    int16_t res = sen5x_get_temperature_offset_parameters(
        &raw_offset, &raw_slope, &raw_time_constant);

    /* Always release the transaction mutex */
    sen54_i2c_transaction_end();

    if (res != 0) {
        ESP_LOGW(TAG, "sen5x_get_temperature_offset_parameters failed: %d", res);
        return ESP_FAIL;
    }

    parameters->offset_c = (float)raw_offset / 200.0f;
    parameters->slope = (float)raw_slope / 10000.0f;
    parameters->time_constant_s = raw_time_constant;

    ESP_LOGI(TAG, "SEN54 temp compensation read: offset=%.3f slope=%.5f tc=%u",
             parameters->offset_c,
             parameters->slope,
             (unsigned)parameters->time_constant_s);

    return ESP_OK;
}
