// Copyright 2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <esp_err.h>
#include <esp_log.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define APP_STORAGE_PARAM_CHECK(con) do { \
        if (!(con)) { \
            ESP_LOGE(TAG, "<ESP_QCLOUD_ERR_INVALID_ARG> !(%s)", #con); \
            return ESP_ERR_INVALID_ARG; \
        } \
    } while(0)

#define APP_STORAGE_ERROR_CHECK(con, err, format, ...) do { \
        if (con) { \
            if(*format != '\0') \
                ESP_LOGW(TAG, "<%s> " format, esp_err_to_name(err), ##__VA_ARGS__); \
            return err; \
        } \
    } while(0)

/** Intialise Storage
 *
 * This API is internally called by app_init(). Applications may call this
 * only if access to the app storage is required before app_init().
 *
 * @return
 *     - ESP_FAIL
 *     - ESP_OK
 */
esp_err_t app_storage_init(void);

/**
 * @brief save the information with given key
 *
 * @param  key    Key name. Maximal length is 15 characters. Shouldn't be empty.
 * @param  value  The value to set.
 * @param  length length of binary value to set, in bytes; Maximum length is
 *                1984 bytes (508000 bytes or (97.6% of the partition size - 4000) bytes
 *                whichever is lower, in case multi-page blob support is enabled).
 *
 * @return
 *     - ESP_FAIL
 *     - ESP_OK
 */
esp_err_t app_storage_set(const char *key, const void *value, size_t length);

/**
 * @brief  Load the information,
 *         esp_err_t app_storage_load(const char *key, void *value, size_t *length);
 *         esp_err_t app_storage_load(const char *key, void *value, size_t length);
 *
 * @attention  The interface of this api supports size_t and size_t * types.
 *             When the length parameter of the pass is size_t, it is only the
 *             length of the value. When the length parameter of the pass is size_t *,
 *             the length of the saved information can be obtained.
 *
 * @param  key    The corresponding key of the information that want to load
 * @param  value  The corresponding value of key
 * @param  length The length of the value, Pointer type will return length
 *
 * @return
 *     - ESP_FAIL
 *     - ESP_OK
 */
esp_err_t app_storage_get(const char *key, void *value, size_t length);

/*
 * @brief  Erase the information with given key
 *
 * @param  key The corresponding key of the information that want to erase
 *
 * @return
 *     - ESP_FAIL
 *     - ESP_OK
 */
esp_err_t app_storage_erase(const char *key);

#ifdef __cplusplus
}
#endif
