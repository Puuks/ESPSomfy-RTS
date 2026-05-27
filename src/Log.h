/* Lightweight logging wrapper for ESPSomfy-RTS
   - Provides `LOGI/LOGW/LOGE/LOGD/LOGV` macros
   - Uses ESP-IDF `esp_log` when available, falls back to `Serial` otherwise
*/
#pragma once

#ifdef ARDUINO_ARCH_ESP32
#include <esp_log.h>
#ifndef LOG_TAG
#define LOG_TAG "ESPSomfy"
#endif
#define LOGI(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) ESP_LOGW(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) ESP_LOGE(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) ESP_LOGD(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGV(fmt, ...) ESP_LOGV(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_HEX(buf, len) esp_log_buffer_hex(LOG_TAG, buf, len)
#else
#include <Arduino.h>
#ifndef LOG_TAG
#define LOG_TAG "ESPSomfy"
#endif
#define LOGI(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#define LOGD(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#define LOGV(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#define LOG_HEX(buf, len) /* no-op on non-ESP */
#endif
