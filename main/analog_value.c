#include "analog_value.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "User_Settings.h"

/* bacnet-stack headers */
#include "bacnet/basic/object/av.h"
#include "esp_err.h"
#include <stdbool.h>

static const char *TAG = "bacnet_av";
#define NVS_NAMESPACE "bacnet"

/* Override NVS values with code defaults - set in main config */
extern int override_nvs_on_flash;

void bacnet_nvs_save_av_name(uint32_t instance, const char *name, uint16_t length) {
    nvs_handle_t nvs_handle;
    char key[32];
    char buf[65] = {0};
    esp_err_t err;
    snprintf(key, sizeof(key), "analog_%lu_name", (unsigned long)instance);
    if (name && length > 0 && length < sizeof(buf)) {
        memcpy(buf, name, length);
        buf[length] = 0;
    }
    if ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle)) == ESP_OK) {
        if ((err = nvs_set_str(nvs_handle, key, buf)) == ESP_OK) {
            if ((err = nvs_commit(nvs_handle)) == ESP_OK) {
                ESP_LOGI(TAG, "Saved AV%lu name: %s", (unsigned long)instance, buf);
            } else {
                ESP_LOGE(TAG, "NVS commit failed for AV%lu name: %d", (unsigned long)instance, err);
            }
        } else {
            ESP_LOGE(TAG, "NVS set_str failed for AV%lu name: %d", (unsigned long)instance, err);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "NVS open failed for AV%lu name: %d", (unsigned long)instance, err);
    }
}

void bacnet_nvs_save_av_desc(uint32_t instance, const char *desc, uint16_t length) {
    nvs_handle_t nvs_handle;
    char key[32];
    char buf[129] = {0};
    esp_err_t err;
    snprintf(key, sizeof(key), "analog_%lu_desc", (unsigned long)instance);
    if (desc && length > 0 && length < sizeof(buf)) {
        memcpy(buf, desc, length);
        buf[length] = 0;
    }
    if ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle)) == ESP_OK) {
        if ((err = nvs_set_str(nvs_handle, key, buf)) == ESP_OK) {
            if ((err = nvs_commit(nvs_handle)) == ESP_OK) {
                ESP_LOGI(TAG, "Saved AV%lu desc: %s", (unsigned long)instance, buf);
            } else {
                ESP_LOGE(TAG, "NVS commit failed for AV%lu desc: %d", (unsigned long)instance, err);
            }
        } else {
            ESP_LOGE(TAG, "NVS set_str failed for AV%lu desc: %d", (unsigned long)instance, err);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "NVS open failed for AV%lu desc: %d", (unsigned long)instance, err);
    }
}

void bacnet_nvs_save_av_units(uint32_t instance, uint16_t units) {
    nvs_handle_t nvs_handle;
    char key[32];
    esp_err_t err;
    snprintf(key, sizeof(key), "analog_%lu_unit", (unsigned long)instance);
    if ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle)) == ESP_OK) {
        if ((err = nvs_set_u16(nvs_handle, key, units)) == ESP_OK) {
            if ((err = nvs_commit(nvs_handle)) == ESP_OK) {
                ESP_LOGI(TAG, "Saved AV%lu units: %u", (unsigned long)instance, units);
            } else {
                ESP_LOGE(TAG, "NVS commit failed for AV%lu units: %d", (unsigned long)instance, err);
            }
        } else {
            ESP_LOGE(TAG, "NVS set_u16 failed for AV%lu units: %d", (unsigned long)instance, err);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "NVS open failed for AV%lu units: %d", (unsigned long)instance, err);
    }
}

void bacnet_nvs_save_av_pv(uint32_t instance, float value) {
    nvs_handle_t nvs_handle;
    char key[32];
    esp_err_t err;
    snprintf(key, sizeof(key), "analog_%lu_val", (unsigned long)instance);
    if ((err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle)) == ESP_OK) {
        /* Read existing value and only write if different to avoid unnecessary NVS commits */
        float existing = 0.0f;
        size_t required = sizeof(existing);
        esp_err_t r = nvs_get_blob(nvs_handle, key, &existing, &required);
        if (r == ESP_OK && required == sizeof(existing)) {
            if (memcmp(&existing, &value, sizeof(value)) == 0) {
                ESP_LOGI(TAG, "AV%lu NVS value unchanged; existing=%.5f requested=%.5f; NVS write skipped",
                         (unsigned long)instance, existing, value);
                nvs_close(nvs_handle);
                return;
            }
        }

        if ((err = nvs_set_blob(nvs_handle, key, &value, sizeof(value))) == ESP_OK) {
            esp_err_t commit_rc = nvs_commit(nvs_handle);
            if (commit_rc == ESP_OK) {
                /* Verify by reading back immediately */
                float verify = 0.0f;
                size_t vlen = sizeof(verify);
                esp_err_t vr = nvs_get_blob(nvs_handle, key, &verify, &vlen);
                if (vr == ESP_OK && vlen == sizeof(verify)) {
                    ESP_LOGI(TAG, "Saved AV%lu value: %.2f (set=%d commit=%d verify=%.2f)",
                             (unsigned long)instance, value, (int)err, (int)commit_rc, verify);
                } else {
                    ESP_LOGE(TAG, "Saved AV%lu but verify failed (set=%d commit=%d verify_rc=%d)",
                             (unsigned long)instance, (int)err, (int)commit_rc, (int)vr);
                }
            } else {
                ESP_LOGE(TAG, "NVS commit failed for AV%lu: %d", (unsigned long)instance, (int)commit_rc);
            }
        } else {
            ESP_LOGE(TAG, "NVS set_blob failed for AV%lu: %d", (unsigned long)instance, (int)err);
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGE(TAG, "NVS open failed for AV%lu: %d", (unsigned long)instance, (int)err);
    }
}

esp_err_t bacnet_nvs_load_av_pv(uint32_t instance, float *value, bool *found)
{
    nvs_handle_t nvs_handle;
    char key[32];
    esp_err_t err;
    if (value) {
        *value = 0.0f;
    }
    if (found) {
        *found = false;
    }

    snprintf(key, sizeof(key), "analog_%lu_val", (unsigned long)instance);
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(float);
    esp_err_t r = nvs_get_blob(nvs_handle, key, value, &len);
    if (r == ESP_OK && len == sizeof(float)) {
        if (found) {
            *found = true;
        }
        nvs_close(nvs_handle);
        return ESP_OK;
    }

    nvs_close(nvs_handle);
    if (r == ESP_ERR_NVS_NOT_FOUND) {
        if (found) {
            *found = false;
        }
        return ESP_OK;
    }

    return r;
}

void bacnet_nvs_load_av(uint32_t instance) {
    nvs_handle_t nvs_handle;
    char key[32];
    static char av_names[USER_AV_COUNT][65];  /* Persistent storage for loaded names */
    static char av_descs[USER_AV_COUNT][129];  /* Persistent storage for loaded descriptions */
    uint8_t idx = (instance > 0 && instance <= USER_AV_COUNT) ? (uint8_t)(instance - 1) : 0;
    uint16_t units = UNITS_PERCENT;
    float pv = 0.0f;
    size_t len;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return;  /* NVS not initialized yet */
    }

    snprintf(key, sizeof(key), "analog_%lu_name", (unsigned long)instance);
    len = sizeof(av_names[idx]);
    if (nvs_get_str(nvs_handle, key, av_names[idx], &len) == ESP_OK) {
        Analog_Value_Name_Set(instance, av_names[idx]);
    }

    snprintf(key, sizeof(key), "analog_%lu_desc", (unsigned long)instance);
    len = sizeof(av_descs[idx]);
    if (nvs_get_str(nvs_handle, key, av_descs[idx], &len) == ESP_OK) {
        Analog_Value_Description_Set(instance, av_descs[idx]);
    }

    snprintf(key, sizeof(key), "analog_%lu_unit", (unsigned long)instance);
    if (nvs_get_u16(nvs_handle, key, &units) == ESP_OK) {
        Analog_Value_Units_Set(instance, units);
    }

    snprintf(key, sizeof(key), "analog_%lu_val", (unsigned long)instance);
    len = sizeof(pv);
    if (nvs_get_blob(nvs_handle, key, &pv, &len) == ESP_OK) {
        Analog_Value_Present_Value_Set(instance, pv, 16);
    }

    nvs_close(nvs_handle);
}

void bacnet_create_analog_values(void) {
    size_t i = 0;
    size_t num_instances = USER_AV_COUNT;

    for (i = 0; i < num_instances; i++) {
        uint32_t instance = USER_AV_INSTANCES[i];
        Analog_Value_Create(instance);
        Analog_Value_Name_Set(instance, USER_AV_NAMES[i]);
        Analog_Value_Description_Set(instance, USER_AV_DESCRIPTIONS[i]);
        Analog_Value_Units_Set(instance, USER_AV_UNITS[i]);
        Analog_Value_Present_Value_Set(instance, USER_AV_INITIAL_VALUES[i], 16);
        Analog_Value_COV_Increment_Set(instance, USER_AV_COV_INCREMENTS[i]);
        Analog_Value_Reliability_Set(instance, RELIABILITY_NO_FAULT_DETECTED);
        Analog_Value_Out_Of_Service_Set(instance, false);
        /* Load persisted values from NVS (if any) - unless override flag is set */
        if (!override_nvs_on_flash) {
            bacnet_nvs_load_av(instance);
        }
    }

    ESP_LOGI(TAG, "Created %zu Analog Value objects", num_instances);
}
