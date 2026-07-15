#include "User_Settings.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "bacnet/bacenum.h"
#include "bacnet/basic/object/device.h"
#include "User_Settings_private.h"

static const char *TAG_USER_SETTINGS = "user_settings";

static const char *user_settings_bool_text(bool value)
{
    return value ? "true" : "false";
}

/* WiFi settings */
#ifndef USER_ENABLE_BACNET_IP_DEFAULT
#define USER_ENABLE_BACNET_IP_DEFAULT false
#endif
const bool USER_ENABLE_BACNET_IP =
    (USER_MSTP_PRODUCTION_TIMING_TEST ? false : USER_ENABLE_BACNET_IP_DEFAULT);
/* Edit these locally if you want to override the hidden defaults from
 * User_Settings_private.h. Keep real credentials out of GitHub.
 */
#ifndef USER_WIFI_SSID
#define USER_WIFI_SSID ""
#endif
#ifndef USER_WIFI_PASS
#define USER_WIFI_PASS ""
#endif
const bool USER_WIFI_USE_STATIC_IP = true;
const char USER_WIFI_STATIC_IP_ADDR[] = "10.120.245.96";
const char USER_WIFI_STATIC_IP_GATEWAY[] = "10.210.245.254";
const char USER_WIFI_STATIC_IP_NETMASK[] = "255.255.255.0";

/* BACnet device settings */
const char USER_BACNET_DEVICE_NAME[] = "ESP32_55533";
const uint32_t USER_BACNET_DEVICE_INSTANCE = 55533; 
const int USER_OVERRIDE_NVS_ON_FLASH = 0; // Set to 1 to override NVS settings on flash with the defaults in User_Settings.c. Set to 0 to use NVS settings on flash if they exist.

/* BACnet device identity settings */
const char USER_DEVICE_DESCRIPTION[] = "ESP32 BACnet Environmental Sensor";
const char USER_DEVICE_MODEL_NAME[] = "ESP32-WROOM32-BACnet-SEN54-HW657A";
const char USER_VENDOR_NAME[] = "ESCAP FMS";
const uint16_t USER_VENDOR_IDENTIFIER = 260;
const char USER_DEVICE_LOCATION[] = "Bangkok";
const char USER_FIRMWARE_REVISION[] = "GMT020-02-7P 2in Display";
const char USER_APPLICATION_SOFTWARE_VERSION[] = "2026_07_14 13:23";
const char USER_DEVICE_SERIAL_NUMBER[] = "ESP32_55533_asdfg"; //CHANGE ME UNIQUE PER DEVICE

void User_Settings_InitDeviceIdentity(void)
{
    Device_Set_Object_Instance_Number(USER_BACNET_DEVICE_INSTANCE);
    Device_Object_Name_ANSI_Init(USER_BACNET_DEVICE_NAME);
    Device_Set_Description(
        USER_DEVICE_DESCRIPTION, strlen(USER_DEVICE_DESCRIPTION));
    Device_Set_Model_Name(
        USER_DEVICE_MODEL_NAME, strlen(USER_DEVICE_MODEL_NAME));
    Device_Set_Vendor_Name(USER_VENDOR_NAME, strlen(USER_VENDOR_NAME));
    Device_Set_Vendor_Identifier(USER_VENDOR_IDENTIFIER);
    Device_Set_Location(USER_DEVICE_LOCATION, strlen(USER_DEVICE_LOCATION));
    Device_Set_Firmware_Revision(
        USER_FIRMWARE_REVISION, strlen(USER_FIRMWARE_REVISION));
    Device_Set_Application_Software_Version(
        USER_APPLICATION_SOFTWARE_VERSION,
        strlen(USER_APPLICATION_SOFTWARE_VERSION));
    Device_Serial_Number_Set(
        USER_DEVICE_SERIAL_NUMBER,
        strlen(USER_DEVICE_SERIAL_NUMBER));
}


/* BBMD foreign device registration */
const uint8_t USER_BBMD_IP_OCTET_1 = 192;
const uint8_t USER_BBMD_IP_OCTET_2 = 168;
const uint8_t USER_BBMD_IP_OCTET_3 = 1;
const uint8_t USER_BBMD_IP_OCTET_4 = 1;
const uint16_t USER_BBMD_PORT = 0xBAC0;
const uint16_t USER_BBMD_TTL_SECONDS = 600;

/* BACnet MS/TP settings */
const bool USER_ENABLE_BACNET_MSTP = true;
const uint8_t USER_MSTP_MAC_ADDRESS = 33;
const uint8_t USER_MSTP_MAX_INFO_FRAMES = 1;
const uint8_t USER_MSTP_MAX_MASTER = 127;
const uint32_t USER_MSTP_BAUD_RATE = 38400U;
const bool USER_MSTP_AUTO_BAUD = false;
#if USER_BACNET_ROUTED_COMPAT_MODE
const uint16_t USER_BACNET_MSTP_MAX_APDU = USER_BACNET_ROUTED_COMPAT_MAX_APDU;
#else
const uint16_t USER_BACNET_MSTP_MAX_APDU = 480;
#endif
const bool USER_MSTP_ENABLE_REPLY_POSTPONED = false;

/* BACnet object defaults */
const uint32_t USER_AV_INSTANCES[USER_AV_COUNT] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
const char *USER_AV_NAMES[USER_AV_COUNT] = {
    "Temp",
    "% RH",
    "PM2.5",
    "VOC Index",
    "PM1.0",
    "PM4",
    "Auto Cleaning Interval",
    "Temperature Offset",
    "Temperature Offset Slope",
    "Temperature Compensation Time Constant",
    "Reserved AV 11",
    "Reserved AV 12",
    "Reserved AV 13",
    "Reserved AV 14",
    "Reserved AV 15",
    "Reserved AV 16"
};
const char *USER_AV_DESCRIPTIONS[USER_AV_COUNT] = {
    "Temperature",
    "Humidity",
    "PM2.5",
    "VOC Index",
    "PM1.0",
    "PM4.0",
    "Automatic fan cleaning interval (seconds)",
    "Temperature compensation offset",
    "Normalized temperature compensation slope",
    "Temperature compensation time constant in seconds",
    "Reserved Analog Value 11",
    "Reserved Analog Value 12",
    "Reserved Analog Value 13",
    "Reserved Analog Value 14",
    "Reserved Analog Value 15",
    "Reserved Analog Value 16"
};
const uint16_t USER_AV_UNITS[USER_AV_COUNT] = {
    UNITS_DEGREES_CELSIUS,
    UNITS_PERCENT,
    UNITS_MICROGRAMS_PER_CUBIC_METER,
    UNITS_NO_UNITS,
    UNITS_MICROGRAMS_PER_CUBIC_METER,
    UNITS_MICROGRAMS_PER_CUBIC_METER,
    UNITS_SECONDS,
    UNITS_DEGREES_CELSIUS,
    UNITS_NO_UNITS,
    UNITS_SECONDS,
    UNITS_DEGREES_CELSIUS,
    UNITS_NO_UNITS,
    UNITS_SECONDS,
    UNITS_NO_UNITS,
    UNITS_NO_UNITS,
    UNITS_NO_UNITS
};
const float USER_AV_INITIAL_VALUES[USER_AV_COUNT] = {
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f
};
const float USER_AV_COV_INCREMENTS[USER_AV_COUNT] = {
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f,
    1.0f
};

const uint32_t USER_BV_INSTANCES[USER_BV_COUNT] = { 1, 2, 3, 4 };
const char *USER_BV_NAMES[USER_BV_COUNT] = {
    "SEN54_Full_Reset",
    "SEN54 Measurement Enable",
    "SEN54 Fan Cleaning",
    "SEN54 Clear Device Status"
};
const char *USER_BV_DESCRIPTIONS[USER_BV_COUNT] = {
    "Write ACTIVE to send I2C reset (0xD304) to SEN54",
    "Enable or disable continuous SEN54 measurements.",
    "Start one manual SEN54 fan cleaning cycle.",
    "Clear SEN54 sticky diagnostic status flags."
};
const char *USER_BV_ACTIVE_TEXT[USER_BV_COUNT] = {
    "RESETTING",
    "Enabled",
    "Start",
    "Clear"
};
const char *USER_BV_INACTIVE_TEXT[USER_BV_COUNT] = {
    "IDLE",
    "Stopped",
    "Idle",
    "Idle"
};
const uint8_t USER_BV_INITIAL_VALUES[USER_BV_COUNT] = {
    BINARY_INACTIVE,
    BINARY_INACTIVE,
    BINARY_INACTIVE,
    BINARY_INACTIVE
};

const uint32_t USER_AI_INSTANCES[USER_AI_COUNT] = { 1 };
const char *USER_AI_NAMES[USER_AI_COUNT] = {
    "Reserved AI 1"
};
const char *USER_AI_DESCRIPTIONS[USER_AI_COUNT] = {
    "Reserved Analog Input 1"
};
const uint16_t USER_AI_UNITS[USER_AI_COUNT] = {
    UNITS_DEGREES_CELSIUS
};
const float USER_AI_INITIAL_VALUES[USER_AI_COUNT] = {
    0.0f
};
const float USER_AI_COV_INCREMENTS[USER_AI_COUNT] = {
    1.0f
};

const uint32_t USER_BI_INSTANCES[USER_BI_COUNT] = { 1, 2, 3, 4 };
const char *USER_BI_NAMES[USER_BI_COUNT] = {
    "SEN54 Fan Failure",
    "SEN54 Laser Error",
    "SEN54 VOC Sensor Error",
    "SEN54 RHT Sensor Error"
};
const char *USER_BI_DESCRIPTIONS[USER_BI_COUNT] = {
    "SEN54 Fan Failure",
    "SEN54 Laser Error",
    "SEN54 VOC Sensor Error",
    "SEN54 RHT Sensor Error"
};
const char *USER_BI_ACTIVE_TEXT[USER_BI_COUNT] = {
    "ACTIVE",
    "ACTIVE",
    "ACTIVE",
    "ACTIVE"
};
const char *USER_BI_INACTIVE_TEXT[USER_BI_COUNT] = {
    "INACTIVE",
    "INACTIVE",
    "INACTIVE",
    "INACTIVE"
};
const uint8_t USER_BI_INITIAL_VALUES[USER_BI_COUNT] = {
    BINARY_INACTIVE,
    BINARY_INACTIVE,
    BINARY_INACTIVE,
    BINARY_INACTIVE
};

const uint32_t USER_BO_INSTANCES[USER_BO_COUNT] = { 1 };
const char *USER_BO_NAMES[USER_BO_COUNT] = {
    "Reserved BO1"
};
const char *USER_BO_DESCRIPTIONS[USER_BO_COUNT] = {
    "Reserved Binary Output 1"
};
const char *USER_BO_ACTIVE_TEXT[USER_BO_COUNT] = {
    "ON"
};
const char *USER_BO_INACTIVE_TEXT[USER_BO_COUNT] = {
    "OFF"
};
const uint8_t USER_BO_INITIAL_VALUES[USER_BO_COUNT] = {
    BINARY_INACTIVE
};

void User_Settings_Print(void)
{
#if USER_SETTINGS_PRINT_ENABLE
    ESP_LOGI(TAG_USER_SETTINGS, "====================================");
    ESP_LOGI(TAG_USER_SETTINGS, "[WiFi Settings]");
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_ENABLE_BACNET_IP", user_settings_bool_text(USER_ENABLE_BACNET_IP));
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_WIFI_SSID", USER_WIFI_SSID);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_WIFI_PASS", "****");
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_WIFI_USE_STATIC_IP", user_settings_bool_text(USER_WIFI_USE_STATIC_IP));
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_WIFI_STATIC_IP_ADDR", USER_WIFI_STATIC_IP_ADDR);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_WIFI_STATIC_IP_GATEWAY", USER_WIFI_STATIC_IP_GATEWAY);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_WIFI_STATIC_IP_NETMASK", USER_WIFI_STATIC_IP_NETMASK);

    ESP_LOGI(TAG_USER_SETTINGS, "====================================");
    ESP_LOGI(TAG_USER_SETTINGS, "[BACnet Device Settings]");
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_BACNET_DEVICE_NAME", USER_BACNET_DEVICE_NAME);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %" PRIu32, "USER_BACNET_DEVICE_INSTANCE", USER_BACNET_DEVICE_INSTANCE);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %d", "USER_OVERRIDE_NVS_ON_FLASH", USER_OVERRIDE_NVS_ON_FLASH);

    ESP_LOGI(TAG_USER_SETTINGS, "====================================");
    ESP_LOGI(TAG_USER_SETTINGS, "[Device Identity Settings]");
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_DEVICE_DESCRIPTION", USER_DEVICE_DESCRIPTION);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_DEVICE_MODEL_NAME", USER_DEVICE_MODEL_NAME);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_VENDOR_NAME", USER_VENDOR_NAME);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %" PRIu16, "USER_VENDOR_IDENTIFIER", USER_VENDOR_IDENTIFIER);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_DEVICE_LOCATION", USER_DEVICE_LOCATION);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_FIRMWARE_REVISION", USER_FIRMWARE_REVISION);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_APPLICATION_SOFTWARE_VERSION", USER_APPLICATION_SOFTWARE_VERSION);
    ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_DEVICE_SERIAL_NUMBER", USER_DEVICE_SERIAL_NUMBER);
 //   ESP_LOGI(TAG_USER_SETTINGS, "%-34s : %s", "USER_MSTP_MAC_ADDRESS", USER_MSTP_MAC_ADDRESS);
    ESP_LOGI(TAG_USER_SETTINGS, "====================================");
#endif
}
