#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK                          0
#define ESP_FAIL                       -1
#define ESP_ERR_NO_MEM                  0x101
#define ESP_ERR_INVALID_ARG             0x102
#define ESP_ERR_INVALID_STATE           0x103
#define ESP_ERR_INVALID_SIZE            0x104
#define ESP_ERR_NOT_FOUND               0x105
#define ESP_ERR_NOT_SUPPORTED           0x106
#define ESP_ERR_TIMEOUT                 0x107
#define ESP_ERR_INVALID_RESPONSE        0x108
#define ESP_ERR_INVALID_VERSION         0x10a
#define ESP_ERR_NOT_ALLOWED             0x10d

static inline const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                  return "OK";
    case ESP_FAIL:                return "FAIL";
    case ESP_ERR_NO_MEM:          return "NO_MEM";
    case ESP_ERR_INVALID_ARG:     return "INVALID_ARG";
    case ESP_ERR_INVALID_STATE:   return "INVALID_STATE";
    case ESP_ERR_TIMEOUT:         return "TIMEOUT";
    case ESP_ERR_NOT_SUPPORTED:   return "NOT_SUPPORTED";
    default:                      return "?";
    }
}

#define ESP_ERROR_CHECK(x)  do { esp_err_t _e = (x); (void)_e; } while (0)
