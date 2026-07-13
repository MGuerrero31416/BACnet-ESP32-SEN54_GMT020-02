#include "sen54.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "sen54_i2c_bridge.h"

static const char *TAG = "SEN54";
static bool i2c_ready = false;
static i2c_master_bus_handle_t sen54_i2c_bus = NULL;
static i2c_master_dev_handle_t sen54_i2c_dev = NULL;

// SEN54 I2C commands (big-endian 16-bit)
// 0x0021: Start Measurement (enables fan and continuous measurements)
// 0x0104: Stop Measurement
// 0x03C4: Read Measured Values
// 0xD304: Device Reset (hardware reset, clears all learned state)
#define SEN54_CMD_START_MEASUREMENT  0x0021
#define SEN54_CMD_STOP_MEASUREMENT   0x0104
#define SEN54_CMD_START_FAN_CLEANING 0x5607
#define SEN54_CMD_CLEAR_DEVICE_STATUS 0xD210
#define SEN54_CMD_READ_VALUES        0x03C4
#define SEN54_CMD_READ_DEVICE_STATUS 0xD206
#define SEN54_CMD_AUTO_CLEANING_INTERVAL 0x8004
// Device Reset (0xD304): forces a full hardware reset of the SEN54.
// All internal state — including the VOC/NOx algorithm learned baselines —
// is cleared. The sensor re-runs its start-up sequence (~1 s) and requires
// a new Start Measurement command (0x0021) before readings resume.
// Equivalent to a power-cycle. See SEN54 datasheet §3.2 "Device Reset".
#define SEN54_CMD_RESET              0xD304

#define SEN54_POWER_ON_DELAY_MS      1000
#define SEN54_CMD_RESPONSE_DELAY_MS  5
#define SEN54_POST_START_DELAY_MS    100
#define SEN54_RESET_SETTLE_DELAY_MS  1200
#define SEN54_START_RETRY_COUNT      20
#define SEN54_MIN_TASK_INTERVAL_MS   1000

// Read Measured Values returns 8 words × 3 bytes (2 data + 1 CRC) = 24 bytes
#define SEN54_READ_VALUES_LEN  24
#define SEN54_READ_STATUS_LEN  6
#define SEN54_READ_AUTO_CLEANING_LEN 6

static bool measurement_enabled = false;

static sen54_data_t current_data = {
    .pm1_0 = -1.0f, .pm2_5 = -1.0f, .pm4_0 = -1.0f, .pm10 = -1.0f,
    .humidity = -1.0f, .temperature = -1.0f, .voc_index = -1.0f, .nox_index = -1.0f
};
static SemaphoreHandle_t sen54_mutex = NULL;
static SemaphoreHandle_t sen54_i2c_mutex = NULL;

/* Bridge functions - declared in sen54_i2c_bridge.h and implemented here */
esp_err_t sen54_i2c_bridge_write(uint8_t address, const uint8_t *data, size_t length);
esp_err_t sen54_i2c_bridge_read(uint8_t address, uint8_t *data, size_t length);
esp_err_t sen54_i2c_transaction_begin(void);
void sen54_i2c_transaction_end(void);

static esp_err_t sen54_write_cmd(uint16_t cmd);
static esp_err_t sen54_read_bytes(uint8_t *buf, size_t len);
static esp_err_t sen54_write_cmd_u32(uint16_t cmd, uint32_t value);

static bool sen54_comm_ready(void)
{
    if (!i2c_ready || !sen54_i2c_dev) {
        ESP_LOGE(TAG, "[SEN54] I2C communication not ready");
        return false;
    }

    return true;
}

static bool sen54_send_simple_command(
    uint16_t cmd,
    const char *success_log,
    const char *failure_log)
{
    esp_err_t ret;

    if (!sen54_comm_ready()) {
        return false;
    }

    ret = sen54_write_cmd(cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s (%s)", failure_log, esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "%s", success_log);
    return true;
}

// ---------------------------------------------------------------------------
// CRC-8 (polynomial 0x31, init 0xFF) used by all Sensirion sensors
// ---------------------------------------------------------------------------
static uint8_t sen54_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Low-level I2C helpers
// ---------------------------------------------------------------------------
static esp_err_t sen54_write_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    if (!sen54_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Protect low-level transfer with recursive I2C mutex */
    if (sen54_i2c_mutex && xSemaphoreTakeRecursive(sen54_i2c_mutex, portMAX_DELAY) == pdTRUE) {
        esp_err_t r = i2c_master_transmit(sen54_i2c_dev, buf, sizeof(buf), 100);
        xSemaphoreGiveRecursive(sen54_i2c_mutex);
        return r;
    }
    return i2c_master_transmit(sen54_i2c_dev, buf, sizeof(buf), 100);
}

static esp_err_t sen54_read_bytes(uint8_t *buf, size_t len)
{
    if (!sen54_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sen54_i2c_mutex && xSemaphoreTakeRecursive(sen54_i2c_mutex, portMAX_DELAY) == pdTRUE) {
        esp_err_t r = i2c_master_receive(sen54_i2c_dev, buf, len, 100);
        xSemaphoreGiveRecursive(sen54_i2c_mutex);
        return r;
    }
    return i2c_master_receive(sen54_i2c_dev, buf, len, 100);
}

static esp_err_t sen54_write_cmd_u32(uint16_t cmd, uint32_t value)
{
    uint16_t msw = (uint16_t)(value >> 16);
    uint16_t lsw = (uint16_t)(value & 0xFFFF);
    uint8_t buf[8];

    if (!sen54_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    buf[0] = (uint8_t)(cmd >> 8);
    buf[1] = (uint8_t)(cmd & 0xFF);

    buf[2] = (uint8_t)(msw >> 8);
    buf[3] = (uint8_t)(msw & 0xFF);
    buf[4] = sen54_crc8(&buf[2], 2);

    buf[5] = (uint8_t)(lsw >> 8);
    buf[6] = (uint8_t)(lsw & 0xFF);
    buf[7] = sen54_crc8(&buf[5], 2);

    if (sen54_i2c_mutex && xSemaphoreTakeRecursive(sen54_i2c_mutex, portMAX_DELAY) == pdTRUE) {
        esp_err_t r = i2c_master_transmit(sen54_i2c_dev, buf, sizeof(buf), 100);
        xSemaphoreGiveRecursive(sen54_i2c_mutex);
        return r;
    }
    return i2c_master_transmit(sen54_i2c_dev, buf, sizeof(buf), 100);
}

// ---------------------------------------------------------------------------
// Parse a word (2 bytes) from a 3-byte CRC-word group, validating CRC.
// Returns false if CRC fails.
// ---------------------------------------------------------------------------
static bool sen54_parse_word(const uint8_t *p, uint16_t *out)
{
    if (sen54_crc8(p, 2) != p[2]) {
        return false;
    }
    *out = ((uint16_t)p[0] << 8) | p[1];
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void sen54_init(void)
{
    esp_err_t err = ESP_OK;
    if (!i2c_ready) {
        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port = SEN54_I2C_PORT,
            .sda_io_num = SEN54_I2C_SDA_PIN,
            .scl_io_num = SEN54_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = 1,
        };
        err = i2c_new_master_bus(&bus_cfg, &sen54_i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            return;
        }

        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = SEN54_I2C_ADDR,
            .scl_speed_hz = SEN54_I2C_FREQ_HZ,
        };
        err = i2c_master_bus_add_device(sen54_i2c_bus, &dev_cfg, &sen54_i2c_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
            return;
        }

        i2c_ready = true;
    }

    // SEN54 needs up to 1 second after power-on before it will ACK on I2C.
    vTaskDelay(pdMS_TO_TICKS(SEN54_POWER_ON_DELAY_MS));

    // Retry start measurement — cold power-up can take several seconds.
    err = ESP_FAIL;
    for (int attempt = 1; attempt <= SEN54_START_RETRY_COUNT; attempt++) {
        err = sen54_write_cmd(SEN54_CMD_START_MEASUREMENT);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SEN54 initialized (attempt %d), measurement started", attempt);
            measurement_enabled = true;

            vTaskDelay(pdMS_TO_TICKS(SEN54_POST_START_DELAY_MS));

            sen54_data_t test_data;
            bool ok = sen54_read(&test_data);
            ESP_LOGI(TAG, "First read result = %s", ok ? "OK" : "FAILED");

            break;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SEN54 not responding at I2C addr 0x%02X (SDA=GPIO%d, SCL=GPIO%d). "
                 "Check wiring and pull-up resistors.",
                 SEN54_I2C_ADDR, SEN54_I2C_SDA_PIN, SEN54_I2C_SCL_PIN);
    }
}

bool sen54_read(sen54_data_t *data)
{
    if (!data) {
        return false;
    }

    // Issue the Read Measured Values command
    esp_err_t err = sen54_write_cmd(SEN54_CMD_READ_VALUES);
    if (err != ESP_OK) {
        // If startup was missed after a cold boot, restart measurement and retry once.
        if (sen54_write_cmd(SEN54_CMD_START_MEASUREMENT) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            err = sen54_write_cmd(SEN54_CMD_READ_VALUES);
        }
        if (err != ESP_OK) {
            return false;
        }
    }

    // Sensor needs a short time to prepare the response
    vTaskDelay(pdMS_TO_TICKS(SEN54_CMD_RESPONSE_DELAY_MS));

    uint8_t buf[SEN54_READ_VALUES_LEN];
    err = sen54_read_bytes(buf, sizeof(buf));
    if (err != ESP_OK) {
        return false;
    }

    // Each word occupies 3 bytes: byte[0], byte[1], CRC
    uint16_t raw_pm1_0, raw_pm2_5, raw_pm4_0, raw_pm10;
    uint16_t raw_hum, raw_temp, raw_voc, raw_nox;

    if (!sen54_parse_word(&buf[0],  &raw_pm1_0) ||
        !sen54_parse_word(&buf[3],  &raw_pm2_5) ||
        !sen54_parse_word(&buf[6],  &raw_pm4_0) ||
        !sen54_parse_word(&buf[9],  &raw_pm10)  ||
        !sen54_parse_word(&buf[12], &raw_hum)   ||
        !sen54_parse_word(&buf[15], &raw_temp)  ||
        !sen54_parse_word(&buf[18], &raw_voc)   ||
        !sen54_parse_word(&buf[21], &raw_nox)) {
        ESP_LOGW(TAG, "CRC error in measurement response");
        return false;
    }

    // Apply scaling factors per SEN54 datasheet
    data->pm1_0        = (raw_pm1_0 == 0xFFFF) ? -1.0f : (float)raw_pm1_0 / 10.0f;
    data->pm2_5        = (raw_pm2_5 == 0xFFFF) ? -1.0f : (float)raw_pm2_5 / 10.0f;
    data->pm4_0        = (raw_pm4_0 == 0xFFFF) ? -1.0f : (float)raw_pm4_0 / 10.0f;
    data->pm10         = (raw_pm10  == 0xFFFF) ? -1.0f : (float)raw_pm10  / 10.0f;
    data->humidity     = ((int16_t)raw_hum  == 0x7FFF) ? -1.0f : (float)(int16_t)raw_hum  / 100.0f;
    data->temperature  = ((int16_t)raw_temp == 0x7FFF) ? -1.0f : (float)(int16_t)raw_temp / 200.0f;
    data->voc_index    = ((int16_t)raw_voc  == 0x7FFF) ? -1.0f : (float)(int16_t)raw_voc  / 10.0f;
    data->nox_index    = ((int16_t)raw_nox  == 0x7FFF) ? -1.0f : (float)(int16_t)raw_nox  / 10.0f;

    return true;
}

bool sen54_read_device_status(uint32_t *status)
{
    if (!status) {
        return false;
    }

    esp_err_t err = sen54_write_cmd(SEN54_CMD_READ_DEVICE_STATUS);
    if (err != ESP_OK) {
        return false;
    }

    // Sensor needs a short time to prepare the response
    vTaskDelay(pdMS_TO_TICKS(SEN54_CMD_RESPONSE_DELAY_MS));

    uint8_t buf[SEN54_READ_STATUS_LEN];
    err = sen54_read_bytes(buf, sizeof(buf));
    if (err != ESP_OK) {
        return false;
    }

    uint16_t msw;
    uint16_t lsw;
    if (!sen54_parse_word(&buf[0], &msw) || !sen54_parse_word(&buf[3], &lsw)) {
        ESP_LOGW(TAG, "CRC error in device status response");
        return false;
    }

    *status = ((uint32_t)msw << 16) | lsw;
    return true;
}

bool sen54_read_auto_cleaning_interval(uint32_t *seconds)
{
    if (!seconds) {
        return false;
    }

    esp_err_t err = sen54_write_cmd(SEN54_CMD_AUTO_CLEANING_INTERVAL);
    if (err != ESP_OK) {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(SEN54_CMD_RESPONSE_DELAY_MS));

    uint8_t buf[SEN54_READ_AUTO_CLEANING_LEN];
    err = sen54_read_bytes(buf, sizeof(buf));
    if (err != ESP_OK) {
        return false;
    }

    uint16_t msw;
    uint16_t lsw;
    if (!sen54_parse_word(&buf[0], &msw) || !sen54_parse_word(&buf[3], &lsw)) {
        ESP_LOGW(TAG, "CRC error in auto cleaning interval response");
        return false;
    }

    *seconds = ((uint32_t)msw << 16) | lsw;
    return true;
}

bool sen54_write_auto_cleaning_interval(uint32_t seconds)
{
    uint32_t verify_seconds = 0;

    esp_err_t err = sen54_write_cmd_u32(SEN54_CMD_AUTO_CLEANING_INTERVAL, seconds);
    if (err != ESP_OK) {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    if (!sen54_read_auto_cleaning_interval(&verify_seconds)) {
        return false;
    }

    if (verify_seconds != seconds) {
        ESP_LOGW(TAG,
            "Auto cleaning interval verify failed: requested=%lu read=%lu",
            (unsigned long)seconds,
            (unsigned long)verify_seconds);
        return false;
    }

    return true;
}

static void sen54_read_task(void *pvParameters)
{
    uint32_t interval_ms = (uint32_t)(uintptr_t)pvParameters;
    sen54_data_t data;

    for (;;) {
        if (sen54_read(&data)) {
            if (sen54_mutex && xSemaphoreTake(sen54_mutex, portMAX_DELAY) == pdTRUE) {
                memcpy(&current_data, &data, sizeof(sen54_data_t));
                xSemaphoreGive(sen54_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

void sen54_start_task(uint32_t interval_ms)
{
    if (interval_ms < SEN54_MIN_TASK_INTERVAL_MS) {
        interval_ms = SEN54_MIN_TASK_INTERVAL_MS;
    }
    if (!sen54_mutex) {
        sen54_mutex = xSemaphoreCreateMutex();
    }
    xTaskCreate(sen54_read_task, "sen54_task", 4096,
                (void *)(uintptr_t)interval_ms, 5, NULL);
}

// ---------------------------------------------------------------------------
// Thread-safe getters
// ---------------------------------------------------------------------------
#define SEN54_GETTER(field) \
    float value = -1.0f; \
    if (sen54_mutex && xSemaphoreTake(sen54_mutex, portMAX_DELAY) == pdTRUE) { \
        value = current_data.field; \
        xSemaphoreGive(sen54_mutex); \
    } else { \
        value = current_data.field; \
    } \
    return value;

float sen54_get_pm1_0(void)       { SEN54_GETTER(pm1_0) }
float sen54_get_pm2_5(void)       { SEN54_GETTER(pm2_5) }
float sen54_get_pm4_0(void)       { SEN54_GETTER(pm4_0) }
float sen54_get_pm10(void)        { SEN54_GETTER(pm10) }
float sen54_get_humidity(void)    { SEN54_GETTER(humidity) }
float sen54_get_temperature(void) { SEN54_GETTER(temperature) }
float sen54_get_voc_index(void)   { SEN54_GETTER(voc_index) }
float sen54_get_nox_index(void)   { SEN54_GETTER(nox_index) }

esp_err_t sen54_full_reset(void)
{
    // Send I2C command 0xD304 (Device Reset).
    // This clears all sensor state including VOC/NOx algorithm baselines.
    // The sensor needs ~1 s to complete its start-up sequence before it
    // will ACK further commands, after which measurement is restarted.
    esp_err_t ret;

    if (!sen54_comm_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = sen54_write_cmd(SEN54_CMD_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[SEN54] Full reset failed (%s)", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[SEN54] Full reset started");
    vTaskDelay(pdMS_TO_TICKS(SEN54_RESET_SETTLE_DELAY_MS));  /* datasheet: device ready after ~1 s */
    ret = sen54_write_cmd(SEN54_CMD_START_MEASUREMENT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[SEN54] Measurement restart after reset failed (%s)",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[SEN54] Measurement started");
        measurement_enabled = true;
    }
    return ret;
}

bool sen54_start_measurement(void)
{
    if (!sen54_send_simple_command(
            SEN54_CMD_START_MEASUREMENT,
            "[SEN54] Measurement started",
            "[SEN54] Measurement start failed")) {
        return false;
    }

    measurement_enabled = true;
    return true;
}

bool sen54_stop_measurement(void)
{
    if (!sen54_send_simple_command(
            SEN54_CMD_STOP_MEASUREMENT,
            "[SEN54] Measurement stopped",
            "[SEN54] Measurement stop failed")) {
        return false;
    }

    measurement_enabled = false;
    return true;
}

bool sen54_start_fan_cleaning(void)
{
    return sen54_send_simple_command(
        SEN54_CMD_START_FAN_CLEANING,
        "[SEN54] Manual fan cleaning started",
        "[SEN54] Manual fan cleaning command failed");
}

bool sen54_clear_device_status(void)
{
    return sen54_send_simple_command(
        SEN54_CMD_CLEAR_DEVICE_STATUS,
        "[SEN54] Device status cleared",
        "[SEN54] Device status clear failed");
}

bool sen54_is_measurement_enabled(void)
{
    return measurement_enabled;
}

void sen54_get_data(sen54_data_t *data)
{
    if (!data) {
        return;
    }
    if (sen54_mutex && xSemaphoreTake(sen54_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(data, &current_data, sizeof(sen54_data_t));
        xSemaphoreGive(sen54_mutex);
    } else {
        memcpy(data, &current_data, sizeof(sen54_data_t));
    }
}

/* ---------------------------------------------------------------------------
 * Bridge and transaction API
 * --------------------------------------------------------------------------- */

esp_err_t sen54_i2c_transaction_begin(void)
{
    if (!sen54_i2c_mutex) {
        sen54_i2c_mutex = xSemaphoreCreateRecursiveMutex();
        if (!sen54_i2c_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTakeRecursive(sen54_i2c_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

void sen54_i2c_transaction_end(void)
{
    if (sen54_i2c_mutex) {
        xSemaphoreGiveRecursive(sen54_i2c_mutex);
    }
}

esp_err_t sen54_i2c_bridge_write(uint8_t address, const uint8_t *data, size_t length)
{
    if (address != SEN54_I2C_ADDR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sen54_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit(sen54_i2c_dev, data, length, 100);
}

esp_err_t sen54_i2c_bridge_read(uint8_t address, uint8_t *data, size_t length)
{
    if (address != SEN54_I2C_ADDR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sen54_i2c_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_receive(sen54_i2c_dev, data, length, 100);
}
