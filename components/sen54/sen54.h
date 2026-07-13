#ifndef SEN54_H
#define SEN54_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// I2C configuration - adjust pins to match your hardware
#define SEN54_I2C_PORT    0          // I2C_NUM_0
#define SEN54_I2C_SDA_PIN 13
#define SEN54_I2C_SCL_PIN 14
#define SEN54_I2C_ADDR    0x69
#define SEN54_I2C_FREQ_HZ 100000

// Sensor data structure
// Values are -1.0f when the sensor reports them as not available
typedef struct {
    float pm1_0;        // PM1.0 concentration (μg/m³)
    float pm2_5;        // PM2.5 concentration (μg/m³)
    float pm4_0;        // PM4.0 concentration (μg/m³)
    float pm10;         // PM10 concentration (μg/m³)
    float humidity;     // Relative humidity (%RH)
    float temperature;  // Temperature (°C)
    float voc_index;    // VOC Index (1–500, dimensionless)
    float nox_index;    // NOx Index (1–500, dimensionless)
} sen54_data_t;

/**
 * @brief Initialize the SEN54 sensor over I2C and start continuous measurement.
 */
void sen54_init(void);

/**
 * @brief Read the latest measured values from the sensor.
 * @param data Pointer to sen54_data_t to fill.
 * @return true if the read succeeded and CRCs passed, false otherwise.
 */
bool sen54_read(sen54_data_t *data);

/**
 * @brief Read the SEN54 Device Status register (0xD206).
 * @param status Pointer to receive the 32-bit status value.
 * @return true if the read succeeded and CRCs passed, false otherwise.
 */
bool sen54_read_device_status(uint32_t *status);

/**
 * @brief Read the SEN54 Auto Cleaning Interval register (0x8004).
 * @param seconds Pointer to receive the interval in seconds.
 * @return true if the read succeeded and CRCs passed, false otherwise.
 */
bool sen54_read_auto_cleaning_interval(uint32_t *seconds);

/**
 * @brief Write the SEN54 Auto Cleaning Interval register (0x8004).
 *
 * Writes the requested interval, reads it back, and verifies the value
 * matches.
 *
 * @param seconds Interval in seconds.
 * @return true if write and read-back verification succeeded, false otherwise.
 */
bool sen54_write_auto_cleaning_interval(uint32_t seconds);

/**
 * @brief Start a background FreeRTOS task that reads the sensor periodically.
 * @param interval_ms Polling interval in milliseconds (minimum 1000).
 */
void sen54_start_task(uint32_t interval_ms);

/* Thread-safe getters for individual measurements */
float sen54_get_pm1_0(void);
float sen54_get_pm2_5(void);
float sen54_get_pm4_0(void);
float sen54_get_pm10(void);
float sen54_get_humidity(void);
float sen54_get_temperature(void);
float sen54_get_voc_index(void);
float sen54_get_nox_index(void);

/**
 * @brief Copy the latest sensor data (thread-safe).
 * @param data Pointer to sen54_data_t to fill.
 */
void sen54_get_data(sen54_data_t *data);

/**
 * @brief Send a full reset command (I2C 0xD304) to the SEN54 and restart measurement.
 *
 * Issues the Device Reset command defined in the SEN54 datasheet §3.2.
 * This resets all internal sensor state — including learned VOC/NOx algorithm
 * baselines — and is equivalent to a power-cycle. The function waits ~1.2 s
 * for the sensor to complete its start-up sequence, then sends Start
 * Measurement (0x0021) so that readings resume automatically.
 *
 * Triggered by BACnet BV1 (SEN54_Full_Reset) being written ACTIVE.
 *
 * @return ESP_OK on success, or an esp_err_t code on I2C failure.
 */
esp_err_t sen54_full_reset(void);

/**
 * @brief Send the Start Measurement command (0x0021) to the SEN54.
 * @return true on success, false on I2C failure.
 */
bool sen54_start_measurement(void);

/**
 * @brief Send the Stop Measurement command (0x0104) to the SEN54.
 * @return true on success, false on I2C failure.
 */
bool sen54_stop_measurement(void);

/**
 * @brief Send the Start Fan Cleaning command (0x5607) to the SEN54.
 * @return true on success, false on I2C failure.
 */
bool sen54_start_fan_cleaning(void);

/**
 * @brief Send the Clear Device Status command (0xD210) to the SEN54.
 * @return true on success, false on I2C failure.
 */
bool sen54_clear_device_status(void);

/**
 * @brief Return the current internal measurement-enabled state.
 * @return true if measurement is active, false if stopped.
 */
bool sen54_is_measurement_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // SEN54_H
