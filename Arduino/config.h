#ifndef CONFIG_H
#define CONFIG_H

#include "hardware_profile.h"

// File system configuration
// #define CONFIG_FILE "/wifi_config.json"

// WiFi and HTTP configuration
#define HTTP_TIMEOUT 50000U // HTTP request timeout in ms
#define RETRY_DELAY 10000U  // Delay between retries in ms
#define MAX_RETRIES 5U      // Maximum number of retry attempts

// GPIO Configuration
#if EPF_HARDWARE_PROFILE == EPF_PROFILE_EE04_EPAPER
#define CONFIG_PIN 2U          // EE04 button 1
#define BATTERY_ADC_PIN 1U     // EE04 A0 / GPIO1
#define BATTERY_ADC_ENABLE_PIN 6U
#else
#define CONFIG_PIN -1          // The OLED prototype has no physical button
#define BATTERY_ADC_PIN -1
#define BATTERY_ADC_ENABLE_PIN -1
#endif

#define BUTTON_DEBOUNCE 100U   // Button debounce time in ms
#define BUTTON_HOLD_TIME 3000U // Button hold time in ms

// Sleep and timing configuration
#define SLEEP_TIME_COMPENSATION 1.009f // Sleep time compensation factor
#define SLEEP_INTERVAL 3600U           // Default sleep interval in seconds (1 hour)
#define MIN_SLEEP_TIME 900U            // Minimum sleep time in seconds (15 minutes)

// Wake up source configuration
#define WAKEUP_PIN GPIO_NUM_2                 // EE04 button 1
#define WAKEUP_LEVEL ESP_GPIO_WAKEUP_GPIO_LOW // Wake up on low level
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define EPF_EXT1_WAKEUP_MODE ESP_EXT1_WAKEUP_ANY_LOW
#else
#define EPF_EXT1_WAKEUP_MODE ESP_EXT1_WAKEUP_ALL_LOW
#endif

// Buffer configuration
#define BUFFER_SIZE 131072U // Buffer size for image processing

#define SERVER_BASE_URL "http://server.ip:15001"
#define PREFERENCES_SLEEP_TIME_KEY "refresh_rate"
#define PREFERENCES_LAST_SLEEP_TIME "last_sleep"
#define PREFERENCES_CONNECT_API_RETRY_COUNT "retry_count"
#define PREFERENCES_CONNECT_WIFI_RETRY_COUNT "wifi_retry"

#define CONFIG_TIMEOUT 300000U // 5 minute

// OLED prototype wiring from sFrame project context.
#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 22
#define OLED_SCREEN_WIDTH 128
#define OLED_SCREEN_HEIGHT 32
#define OLED_RESET_PIN -1
#define OLED_I2C_ADDRESS 0x3C
#define OLED_MAX_CHARS (OLED_SCREEN_WIDTH / 6)

#endif // CONFIG_H
