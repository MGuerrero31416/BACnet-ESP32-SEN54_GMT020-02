#include "pms5003.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "PMS5003";

// PMS5003 protocol constants
#define PMS5003_FRAME_LENGTH 32
#define PMS5003_START_BYTE1 0x42
#define PMS5003_START_BYTE2 0x4D

/**
 * @brief Initialize PMS5003 sensor
 */
void pms5003_init(void)
{
    // ESP_LOGI(TAG, "Initializing PMS5003 sensor...");
    
    // Configure UART2 (GPIO 16 RX, GPIO 17 TX)
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    
    uart_param_config(PMS5003_UART_NUM, &uart_config);
    uart_set_pin(PMS5003_UART_NUM, PMS5003_TX_PIN, PMS5003_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(PMS5003_UART_NUM, 128, 0, 0, NULL, 0);
    
    // ESP_LOGI(TAG, "UART2 configured (RX=GPIO%d, TX=GPIO%d, Baud=9600)", PMS5003_RX_PIN, PMS5003_TX_PIN);
    
    // Configure SET pin as output for sleep mode control
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PMS5003_SET_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    gpio_config(&io_conf);
    gpio_set_level(PMS5003_SET_PIN, 1);  // Set to high (active mode)
    
    // ESP_LOGI(TAG, "SET pin configured (GPIO%d)", PMS5003_SET_PIN);
    // ESP_LOGI(TAG, "PMS5003 initialization complete");
}

/**
 * @brief Calculate checksum for PMS5003 frame
 */
static uint16_t pms5003_calculate_checksum(const uint8_t *frame)
{
    uint16_t checksum = 0;
    for (int i = 0; i < 30; i++) {
        checksum += frame[i];
    }
    return checksum;
}

/**
 * @brief Read data from PMS5003 sensor
 */
bool pms5003_read(pms5003_data_t *data)
{
    uint8_t frame[PMS5003_FRAME_LENGTH];
    int len = 0;
    
    // Read data from UART
    len = uart_read_bytes(PMS5003_UART_NUM, frame, PMS5003_FRAME_LENGTH, 1000 / portTICK_PERIOD_MS);
    
    if (len != PMS5003_FRAME_LENGTH) {
        // ESP_LOGW(TAG, "Incomplete frame received. Expected %d bytes, got %d", PMS5003_FRAME_LENGTH, len);
        return false;
    }
    
    // Verify frame header
    if (frame[0] != PMS5003_START_BYTE1 || frame[1] != PMS5003_START_BYTE2) {
        // ESP_LOGW(TAG, "Invalid frame header: 0x%02X 0x%02X", frame[0], frame[1]);
        return false;
    }
    
    // Verify checksum
    uint16_t calculated_checksum = pms5003_calculate_checksum(frame);
    uint16_t frame_checksum = (frame[30] << 8) | frame[31];
    
    if (calculated_checksum != frame_checksum) {
        // ESP_LOGW(TAG, "Checksum mismatch: calculated=0x%04X, frame=0x%04X", calculated_checksum, frame_checksum);
        return false;
    }
    
    // Extract sensor data from frame
    data->pm1_0 = (frame[3] << 8) | frame[4];
    data->pm2_5 = (frame[5] << 8) | frame[6];
    data->pm10 = (frame[7] << 8) | frame[8];
    
    data->pm1_0_atm = (frame[9] << 8) | frame[10];
    data->pm2_5_atm = (frame[11] << 8) | frame[12];
    data->pm10_atm = (frame[13] << 8) | frame[14];
    
    data->particles_0_3 = (frame[15] << 8) | frame[16];
    data->particles_0_5 = (frame[17] << 8) | frame[18];
    data->particles_1_0 = (frame[19] << 8) | frame[20];
    data->particles_2_5 = (frame[21] << 8) | frame[22];
    data->particles_5_0 = (frame[23] << 8) | frame[24];
    data->particles_10_0 = (frame[25] << 8) | frame[26];
    
    return true;
}

/**
 * @brief Put sensor into sleep mode
 */
void pms5003_sleep(void)
{
    // ESP_LOGI(TAG, "PMS5003 entering sleep mode");
    gpio_set_level(PMS5003_SET_PIN, 0);  // Set LOW for sleep mode
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

/**
 * @brief Wake sensor from sleep mode
 */
void pms5003_wake(void)
{
    // ESP_LOGI(TAG, "PMS5003 waking from sleep mode");
    gpio_set_level(PMS5003_SET_PIN, 1);  // Set HIGH for active mode
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

/**
 * @brief Print sensor data to serial monitor
 */
void pms5003_print_data(const pms5003_data_t *data)
{
    // printf("\n========== PMS5003 Sensor Data ==========\n");
    // printf("PM1.0:  %u μg/m³ (STD: %u μg/m³)\n", data->pm1_0, data->pm1_0_atm);
    // printf("PM2.5:  %u μg/m³ (STD: %u μg/m³)\n", data->pm2_5, data->pm2_5_atm);
    // printf("PM10:   %u μg/m³ (STD: %u μg/m³)\n", data->pm10, data->pm10_atm);
    // printf("\nParticle counts (per 0.1L air):\n");
    // printf("  >0.3μm:  %u\n", data->particles_0_3);
    // printf("  >0.5μm:  %u\n", data->particles_0_5);
    // printf("  >1.0μm:  %u\n", data->particles_1_0);
    // printf("  >2.5μm:  %u\n", data->particles_2_5);
    // printf("  >5.0μm:  %u\n", data->particles_5_0);
    // printf("  >10.0μm: %u\n", data->particles_10_0);
    // printf("=========================================\n\n");
}
