#ifndef PMS5003_H
#define PMS5003_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// GPIO Configuration
#define PMS5003_RX_PIN 5      // ESP32 RX on GPIO5 (sensor TX)
#define PMS5003_TX_PIN 4      // ESP32 TX on GPIO4 (sensor RX)
#define PMS5003_SET_PIN 6     // PMS5003 RST (reset control)
#define PMS5003_UART_NUM UART_NUM_1

// Sensor data structure - matches PMS5003 frame layout exactly
typedef struct {
    uint16_t framelen;        // Frame length (should be 28)
    uint16_t pm1_0;           // PM1.0 concentration (μg/m³)
    uint16_t pm2_5;           // PM2.5 concentration (μg/m³)
    uint16_t pm10;            // PM10 concentration (μg/m³)
    uint16_t pm1_0_atm;       // PM1.0 concentration under standard particle (μg/m³)
    uint16_t pm2_5_atm;       // PM2.5 concentration under standard particle (μg/m³)
    uint16_t pm10_atm;        // PM10 concentration under standard particle (μg/m³)
    uint16_t particles_0_3;   // Particles > 0.3μm in 0.1L air
    uint16_t particles_0_5;   // Particles > 0.5μm in 0.1L air
    uint16_t particles_1_0;   // Particles > 1.0μm in 0.1L air
    uint16_t particles_2_5;   // Particles > 2.5μm in 0.1L air
    uint16_t particles_5_0;   // Particles > 5.0μm in 0.1L air
    uint16_t particles_10_0;  // Particles > 10.0μm in 0.1L air
    uint16_t unused;          // Unused byte
    uint16_t checksum;        // Checksum
} pms5003_data_t;

/**
 * @brief Initialize PMS5003 sensor
 * Sets up UART and GPIO for reset pin control
 */
void pms5003_init(void);

/**
 * @brief Read data from PMS5003 sensor
 * @param data Pointer to pms5003_data_t structure to store readings
 * @return true if data read successfully, false otherwise
 */
bool pms5003_read(pms5003_data_t *data);

/**
 * @brief Put sensor into sleep mode
 */
void pms5003_sleep(void);

/**
 * @brief Wake sensor from sleep mode
 */
void pms5003_wake(void);

/**
 * @brief Print sensor data to serial monitor
 * @param data Pointer to pms5003_data_t structure with sensor readings
 */
void pms5003_print_data(const pms5003_data_t *data);

/**
 * @brief Start continuous sensor reading task
 * Reads sensor data every interval_ms milliseconds
 * @param interval_ms Interval between readings in milliseconds (minimum 1000)
 */
void pms5003_start_task(uint32_t interval_ms);

/**
 * @brief Get current PM2.5 value (thread-safe)
 * @return PM2.5 concentration in μg/m³
 */
uint16_t pms5003_get_pm2_5(void);

/**
 * @brief Get current PM1.0 value (thread-safe)
 * @return PM1.0 concentration in μg/m³
 */
uint16_t pms5003_get_pm1_0(void);

/**
 * @brief Get current PM10 value (thread-safe)
 * @return PM10 concentration in μg/m³
 */
uint16_t pms5003_get_pm10(void);

/**
 * @brief Get all current sensor data (thread-safe)
 * @param data Pointer to pms5003_data_t structure to fill
 */
void pms5003_get_data(pms5003_data_t *data);

/**
 * @brief Control PMS5003 reset pin from BACnet Binary Output
 * @param state 0 (BINARY_INACTIVE/OFF) = RUN (HIGH), 1 (BINARY_ACTIVE/ON) = RESET (LOW)
 */
void pms5003_set_gpio_from_bo(uint32_t state);

#ifdef __cplusplus
}
#endif

#endif // PMS5003_H

