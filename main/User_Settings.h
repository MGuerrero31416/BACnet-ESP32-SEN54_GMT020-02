#ifndef USER_SETTINGS_H
#define USER_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifndef USER_SETTINGS_PRINT_ENABLE
#define USER_SETTINGS_PRINT_ENABLE 1
#endif

#ifndef USER_MSTP_PRODUCTION_TIMING_TEST
#define USER_MSTP_PRODUCTION_TIMING_TEST 1
#endif

/* Compile-time switch to enable/disable the SEN54 hardware interface.
 * Set to 0 to disable the SEN54 driver and task (reversible).
 */
#if USER_MSTP_PRODUCTION_TIMING_TEST
#define BOARD_SEN54_ENABLED 0
#else
#define BOARD_SEN54_ENABLED 1
#endif

#ifndef USER_BACNET_ROUTED_COMPAT_MODE
#define USER_BACNET_ROUTED_COMPAT_MODE 0
#endif

#ifndef USER_BACNET_ROUTED_COMPAT_MAX_APDU
#define USER_BACNET_ROUTED_COMPAT_MAX_APDU 128U
#endif

/* WiFi settings */
extern const bool USER_ENABLE_BACNET_IP;
extern const char USER_WIFI_SSID[];
extern const char USER_WIFI_PASS[];
extern const bool USER_WIFI_USE_STATIC_IP;
extern const char USER_WIFI_STATIC_IP_ADDR[];
extern const char USER_WIFI_STATIC_IP_GATEWAY[];
extern const char USER_WIFI_STATIC_IP_NETMASK[];

/* BACnet device settings */
extern const char USER_BACNET_DEVICE_NAME[];
extern const uint32_t USER_BACNET_DEVICE_INSTANCE;
extern const int USER_OVERRIDE_NVS_ON_FLASH;

/* BACnet device identity settings */
extern const char USER_DEVICE_DESCRIPTION[];
extern const char USER_DEVICE_MODEL_NAME[];
extern const char USER_VENDOR_NAME[];
extern const uint16_t USER_VENDOR_IDENTIFIER;
extern const char USER_DEVICE_LOCATION[];
extern const char USER_FIRMWARE_REVISION[];
extern const char USER_APPLICATION_SOFTWARE_VERSION[];
extern const char USER_DEVICE_SERIAL_NUMBER[];

void User_Settings_InitDeviceIdentity(void);
void User_Settings_Print(void);

/* BBMD foreign device registration */
extern const uint8_t USER_BBMD_IP_OCTET_1;
extern const uint8_t USER_BBMD_IP_OCTET_2;
extern const uint8_t USER_BBMD_IP_OCTET_3;
extern const uint8_t USER_BBMD_IP_OCTET_4;
extern const uint16_t USER_BBMD_PORT;
extern const uint16_t USER_BBMD_TTL_SECONDS;

/* BACnet MS/TP settings */
extern const bool USER_ENABLE_BACNET_MSTP;
extern const uint8_t USER_MSTP_MAC_ADDRESS;
extern const uint8_t USER_MSTP_MAX_INFO_FRAMES;
extern const uint8_t USER_MSTP_MAX_MASTER;
extern const uint32_t USER_MSTP_BAUD_RATE;
extern const bool USER_MSTP_AUTO_BAUD;
extern const uint16_t USER_BACNET_MSTP_MAX_APDU;
extern const bool USER_MSTP_ENABLE_REPLY_POSTPONED;

/* BACnet object defaults */
#define USER_AV_COUNT 16
#define USER_BV_COUNT 4
#define USER_AI_COUNT 1
#define USER_BI_COUNT 4
#define USER_BO_COUNT 1

extern const uint32_t USER_AV_INSTANCES[USER_AV_COUNT];
extern const char *USER_AV_NAMES[USER_AV_COUNT];
extern const char *USER_AV_DESCRIPTIONS[USER_AV_COUNT];
extern const uint16_t USER_AV_UNITS[USER_AV_COUNT];
extern const float USER_AV_INITIAL_VALUES[USER_AV_COUNT];
extern const float USER_AV_COV_INCREMENTS[USER_AV_COUNT];

extern const uint32_t USER_BV_INSTANCES[USER_BV_COUNT];
extern const char *USER_BV_NAMES[USER_BV_COUNT];
extern const char *USER_BV_DESCRIPTIONS[USER_BV_COUNT];
extern const char *USER_BV_ACTIVE_TEXT[USER_BV_COUNT];
extern const char *USER_BV_INACTIVE_TEXT[USER_BV_COUNT];
extern const uint8_t USER_BV_INITIAL_VALUES[USER_BV_COUNT];

extern const uint32_t USER_AI_INSTANCES[USER_AI_COUNT];
extern const char *USER_AI_NAMES[USER_AI_COUNT];
extern const char *USER_AI_DESCRIPTIONS[USER_AI_COUNT];
extern const uint16_t USER_AI_UNITS[USER_AI_COUNT];
extern const float USER_AI_INITIAL_VALUES[USER_AI_COUNT];
extern const float USER_AI_COV_INCREMENTS[USER_AI_COUNT];

extern const uint32_t USER_BI_INSTANCES[USER_BI_COUNT];
extern const char *USER_BI_NAMES[USER_BI_COUNT];
extern const char *USER_BI_DESCRIPTIONS[USER_BI_COUNT];
extern const char *USER_BI_ACTIVE_TEXT[USER_BI_COUNT];
extern const char *USER_BI_INACTIVE_TEXT[USER_BI_COUNT];
extern const uint8_t USER_BI_INITIAL_VALUES[USER_BI_COUNT];

extern const uint32_t USER_BO_INSTANCES[USER_BO_COUNT];
extern const char *USER_BO_NAMES[USER_BO_COUNT];
extern const char *USER_BO_DESCRIPTIONS[USER_BO_COUNT];
extern const char *USER_BO_ACTIVE_TEXT[USER_BO_COUNT];
extern const char *USER_BO_INACTIVE_TEXT[USER_BO_COUNT];
extern const uint8_t USER_BO_INITIAL_VALUES[USER_BO_COUNT];

#endif /* USER_SETTINGS_H */
