#include "wifi_helper.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "User_Settings.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static const char *TAG = "wifi_helper";
static EventGroupHandle_t s_wifi_event_group;
static bool s_ip_logged;
static uint8_t s_retry_count;
static bool s_reconnect_stopped;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CFG_NAMESPACE "wifi_cfg"
#define WIFI_MAX_RETRY_ATTEMPTS 3

static bool wifi_has_text(const char *text)
{
    return text && text[0] != '\0';
}

static bool wifi_load_credentials_nvs(char *ssid, size_t ssid_len,
                                      char *password, size_t password_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = password_len;
    err = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (err != ESP_OK || ssid_size <= 1) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, "pass", password, &pass_size);
    if (err != ESP_OK) {
        password[0] = '\0';
    }

    nvs_close(handle);
    return true;
}

esp_err_t wifi_save_credentials_nvs(const char *ssid, const char *password)
{
    if (!wifi_has_text(ssid)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "pass", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_logged = false;

        if (s_reconnect_stopped) {
            return;
        }

        if (s_retry_count < WIFI_MAX_RETRY_ATTEMPTS) {
            s_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "WiFi disconnected, retrying connection (%u/%u)",
                     (unsigned)s_retry_count,
                     (unsigned)WIFI_MAX_RETRY_ATTEMPTS);
        } else {
            s_reconnect_stopped = true;
            ESP_LOGW(TAG, "WiFi reconnect limit reached; stopping retries");
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event && !s_ip_logged) {
            ESP_LOGI(TAG, "WiFi connected with IP: " IPSTR, IP2STR(&event->ip_info.ip));
            wifi_config_t cfg = { 0 };
            if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
                wifi_has_text((const char *)cfg.sta.ssid)) {
                ESP_LOGI(TAG, "WiFi connected to SSID: %s", (const char *)cfg.sta.ssid);
            } else {
                ESP_LOGI(TAG, "WiFi connected (SSID unknown)");
            }
            s_ip_logged = true;
        }
        esp_wifi_set_ps(WIFI_PS_NONE);   // disable modem sleep – prevents ping timeouts
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        s_retry_count = 0;
        s_reconnect_stopped = false;
    }
}

/* Simple Wi-Fi setup (blocking with bounded retries) */
wifi_startup_status_t wifi_init_sta(void)
{
    /* Initialize networking stack (only call once in app_main context) */
    /* esp_netif_init() and esp_event_loop_create_default() should be called before this */
    s_wifi_event_group = xEventGroupCreate();
    s_ip_logged = false;
    s_retry_count = 0;
    s_reconnect_stopped = false;

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    // Keep credentials in flash so device can use provisioned Wi-Fi after reboot.
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);

    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &wifi_event_instance);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        &ip_event_instance);
    ESP_LOGI(TAG, "WiFi event hooks registered (WIFI_EVENT, IP_EVENT_STA_GOT_IP)");

    esp_netif_t *esp_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (USER_WIFI_USE_STATIC_IP && esp_netif) {
        esp_netif_dhcpc_stop(esp_netif);
        esp_netif_ip_info_t ip_info = {0};
        ip4_addr_t ip4 = {0};
        if (ip4addr_aton(USER_WIFI_STATIC_IP_ADDR, &ip4)) {
            ip_info.ip.addr = ip4.addr;
        }
        if (ip4addr_aton(USER_WIFI_STATIC_IP_GATEWAY, &ip4)) {
            ip_info.gw.addr = ip4.addr;
        }
        if (ip4addr_aton(USER_WIFI_STATIC_IP_NETMASK, &ip4)) {
            ip_info.netmask.addr = ip4.addr;
        }
        esp_netif_set_ip_info(esp_netif, &ip_info);
        ESP_LOGI(TAG, "Using static IP %s", USER_WIFI_STATIC_IP_ADDR);
    }

    char ssid[33] = {0};
    char password[65] = {0};
    bool have_credentials = false;

    if (wifi_load_credentials_nvs(ssid, sizeof(ssid), password, sizeof(password))) {
        have_credentials = true;
        ESP_LOGI(TAG, "Using Wi-Fi credentials from NVS namespace '%s'", WIFI_CFG_NAMESPACE);
    } else if (wifi_has_text(USER_WIFI_SSID)) {
        strncpy(ssid, USER_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, USER_WIFI_PASS, sizeof(password) - 1);
        have_credentials = true;
        ESP_LOGI(TAG, "Using compile-time Wi-Fi defaults");
    } else {
        ESP_LOGI(TAG, "No app Wi-Fi defaults set; trying provisioned credentials in Wi-Fi flash storage");
    }

    wifi_config_t wifi_config = { 0 };
    if (have_credentials) {
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        if (!wifi_has_text(password)) {
            // Only permit open auth when intentionally configured with empty password.
            wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (have_credentials) {
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);   // disable modem sleep before connecting

    if (have_credentials) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi %s ...", ssid);
    } else {
        ESP_LOGI(TAG, "Connecting to provisioned Wi-Fi credentials ...");
    }
    esp_wifi_connect();

    EventBits_t bits = 0;
    if (s_wifi_event_group) {
        bits = xEventGroupWaitBits(s_wifi_event_group,
                                   WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(15000));
    }

    if (bits & WIFI_CONNECTED_BIT) {
        return WIFI_STARTUP_CONNECTED;
    }

    if ((bits & WIFI_FAILED_BIT) == 0) {
        s_reconnect_stopped = true;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
        }
        ESP_LOGW(TAG, "WiFi connection timeout - stopping retries");
    }

    return WIFI_STARTUP_UNAVAILABLE;
}
