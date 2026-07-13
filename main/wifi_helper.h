#ifndef WIFI_HELPER_H
#define WIFI_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

typedef enum {
	WIFI_STARTUP_UNAVAILABLE = 0,
	WIFI_STARTUP_CONNECTED = 1
} wifi_startup_status_t;

/**
 * Initialize Wi-Fi in station mode and try to connect to the configured network.
 * Returns connection status after bounded retry attempts.
 */
wifi_startup_status_t wifi_init_sta(void);

/**
 * Store Wi-Fi credentials in application NVS namespace.
 * These values take priority over compile-time defaults on next boot.
 */
esp_err_t wifi_save_credentials_nvs(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_HELPER_H */
