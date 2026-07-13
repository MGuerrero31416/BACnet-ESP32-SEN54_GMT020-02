#include "analog_input.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "User_Settings.h"

/* bacnet-stack headers */
#include "bacnet/basic/object/ai.h"

static const char *TAG = "bacnet_ai";
#define NVS_NAMESPACE "bacnet"

/* Override NVS values with code defaults - set in main config */
extern int override_nvs_on_flash;

void bacnet_nvs_save_ai_name(uint32_t instance, const char *name, uint16_t length) {
    nvs_handle_t nvs_handle;
    char key[32];
    char buf[65] = {0};
    esp_err_t err;
    snprintf(key, sizeof(key), "ai_%lu_name", (unsigned long)instance);
    if (name && length > 0 && length < sizeof(buf)) {
        memcpy(buf, name, length);
        buf[length] = 0;
    }
    if ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle)) == ESP_OK) {
        if ((err = nvs_set_str(nvs_handle, key, buf)) == ESP_OK) {
            if ((err = nvs_commit(nvs_handle)) == ESP_OK) {
                ESP_LOGI(TAG, "Saved AI%lu name: %s", (unsigned long)instance, buf);
            } else {
                ESP_LOGE(TAG, "NVS commit failed for AI%lu name: %d", (unsigned long)instance, err);
            }
        } else {
            ESP_LOGE(TAG, "NVS set_str failed for AI%lu name: %d", (unsigned long)instance, err);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "NVS open failed for AI%lu name: %d", (unsigned long)instance, err);
    }
}

void bacnet_nvs_save_ai_desc(uint32_t instance, const char *desc, uint16_t length) {
    nvs_handle_t nvs_handle;
    char key[32];
    char buf[129] = {0};
    esp_err_t err;
    snprintf(key, sizeof(key), "ai_%lu_desc", (unsigned long)instance);
    if (desc && length > 0 && length < sizeof(buf)) {
        memcpy(buf, desc, length);
        buf[length] = 0;
    }
    if ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle)) == ESP_OK) {
        if ((err = nvs_set_str(nvs_handle, key, buf)) == ESP_OK) {
            if ((err = nvs_commit(nvs_handle)) == ESP_OK) {
                ESP_LOGI(TAG, "Saved AI%lu desc: %s", (unsigned long)instance, buf);
            } else {
                ESP_LOGE(TAG, "NVS commit failed for AI%lu desc: %d", (unsigned long)instance, err);
            }
        } else {
            ESP_LOGE(TAG, "NVS set_str failed for AI%lu desc: %d", (unsigned long)instance, err);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "NVS open failed for AI%lu desc: %d", (unsigned long)instance, err);
    }
}

void bacnet_nvs_save_ai_pv(uint32_t instance, float value) {
    nvs_handle_t nvs_handle;
    char key[32];
    esp_err_t err;
    snprintf(key, sizeof(key), "ai_%lu_val", (unsigned long)instance);
    if ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle)) == ESP_OK) {
        if ((err = nvs_set_blob(nvs_handle, key, &value, sizeof(float))) == ESP_OK) {
            if ((err = nvs_commit(nvs_handle)) == ESP_OK) {
                ESP_LOGI(TAG, "Saved AI%lu value: %.2f", (unsigned long)instance, value);
            } else {
                ESP_LOGE(TAG, "NVS commit failed for AI%lu value: %d", (unsigned long)instance, err);
            }
        } else {
            ESP_LOGE(TAG, "NVS set_blob failed for AI%lu value: %d", (unsigned long)instance, err);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "NVS open failed for AI%lu value: %d", (unsigned long)instance, err);
    }
}

void bacnet_nvs_load_ai(uint32_t instance) {
    nvs_handle_t nvs_handle;
    char key[32];
    static char ai_names[USER_AI_COUNT][65];  /* Persistent storage for loaded names */
    static char ai_descs[USER_AI_COUNT][129];  /* Persistent storage for loaded descriptions */
    uint8_t idx = (instance > 0 && instance <= USER_AI_COUNT) ? (uint8_t)(instance - 1) : 0;
    float pv = 0.0f;
    size_t len;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return;  /* NVS not initialized yet */
    }

    snprintf(key, sizeof(key), "ai_%lu_name", (unsigned long)instance);
    len = sizeof(ai_names[idx]);
    if (nvs_get_str(nvs_handle, key, ai_names[idx], &len) == ESP_OK) {
        Analog_Input_Name_Set(instance, ai_names[idx]);
    }

    snprintf(key, sizeof(key), "ai_%lu_desc", (unsigned long)instance);
    len = sizeof(ai_descs[idx]);
    if (nvs_get_str(nvs_handle, key, ai_descs[idx], &len) == ESP_OK) {
        Analog_Input_Description_Set(instance, ai_descs[idx]);
    }

    snprintf(key, sizeof(key), "ai_%lu_val", (unsigned long)instance);
    len = sizeof(float);
    if (nvs_get_blob(nvs_handle, key, &pv, &len) == ESP_OK) {
        Analog_Input_Present_Value_Set(instance, pv);
    }

    nvs_close(nvs_handle);
}

void bacnet_create_analog_inputs(void) {
    size_t i = 0;
    size_t num_instances = USER_AI_COUNT;

    for (i = 0; i < num_instances; i++) {
        uint32_t instance = USER_AI_INSTANCES[i];
        Analog_Input_Create(instance);
        Analog_Input_Name_Set(instance, USER_AI_NAMES[i]);
        Analog_Input_Description_Set(instance, USER_AI_DESCRIPTIONS[i]);
        Analog_Input_Units_Set(instance, USER_AI_UNITS[i]);
        Analog_Input_Present_Value_Set(instance, USER_AI_INITIAL_VALUES[i]);
        Analog_Input_Reliability_Set(instance, RELIABILITY_NO_FAULT_DETECTED);
        Analog_Input_Out_Of_Service_Set(instance, false);
        /* Load persisted values from NVS (if any) - unless override flag is set */
        if (!override_nvs_on_flash) {
            bacnet_nvs_load_ai(instance);
        }
    }

    ESP_LOGI(TAG, "Created %zu Analog Input objects", num_instances);
}
