#include <Arduino.h>
#include <SPI.h>
#include <HTTPClient.h>
#include "hardware_profile.h"
#if EPF_ENABLE_TAILSCALE
#include <microlink.h>
#endif
#if EPF_USE_EPAPER
#include "epd7in3e.h"
#endif
#include "FS.h"
#include <ArduinoJson.h>
// #include "SimpleWiFiManager.h"
#if EPF_ENABLE_SECURE_HTTP
#include <WiFiClientSecure.h>
#endif
#include "driver/rtc_io.h"
#include "config.h"
#include "button.h"
#include <Preferences.h>
#include <WifiCaptive.h>
#include <filesystem.h>
#if EPF_USE_OLED
#include "oled_status.h"
#endif

Preferences preferences;

#if EPF_ENABLE_TAILSCALE
static microlink_t *tailnet = nullptr;

static void onTailscaleState(microlink_t *handle, microlink_state_t state, void *)
{
  const char *names[] = {
      "idle", "waiting for WiFi", "connecting", "registering",
      "connected", "reconnecting", "error"};
  const char *name = state < (sizeof(names) / sizeof(names[0])) ? names[state] : "unknown";
  Serial.printf("[tailscale] state: %s\n", name);

  if (state == ML_STATE_CONNECTED)
  {
    char ip[16] = {};
    microlink_ip_to_str(microlink_get_vpn_ip(handle), ip);
    Serial.printf("[tailscale] VPN address: %s\n", ip);
  }
}
#endif

class EpaperManager
{
private:
  // SimpleWiFiManager wifiManager;
#if EPF_USE_EPAPER
  Epd epd;
#endif
#if EPF_USE_OLED
  OledStatus oled;
#endif
  String imageUrl = "";
  String receivedImageName = "";
  int requestedSleepSeconds = 0;
  bool tailnetReady = false;

  void showStatus(const String &line1, const String &line2 = String())
  {
#if EPF_USE_OLED
    oled.show(line1, line2);
#endif
    Serial.print("[status] ");
    Serial.print(line1);
    if (line2.length() > 0)
    {
      Serial.print(" | ");
      Serial.print(line2);
    }
    Serial.println();
  }

  bool downloadImage()
  {
    receivedImageName = "";
    // An empty saved preference must not hide the compile-time default. This
    // can happen when the captive portal was submitted without a server URL.
    imageUrl = preferences.getString("SERVER_BASE_URL", "");
    if (imageUrl.length() == 0)
    {
      imageUrl = SERVER_BASE_URL;
    }
    if (imageUrl.length() == 0)
    {
      showStatus("No server URL", "Configure portal");
      return false;
    }

    showStatus("Fetching image", imageUrl);
    Serial.print("nas url: ");
    Serial.println(imageUrl);
    bool isHttps = imageUrl.startsWith("https://");
#if !EPF_ENABLE_SECURE_HTTP
    if (isHttps)
    {
#if EPF_ENABLE_TAILSCALE
      showStatus("Use Tailscale HTTP", "Tunnel is encrypted");
      Serial.println(F("Use an http://100.x.x.x:15001 server URL for the Tailscale path."));
#else
      showStatus("Use HTTP URL", "HTTPS unavailable");
      Serial.println(F("Use an http:// server URL for the ESP-IDF local profile."));
#endif
      return false;
    }
#endif
    WiFiClient *basicClient = nullptr;
#if EPF_ENABLE_SECURE_HTTP
    WiFiClientSecure *secureClient = nullptr;
#endif
    HTTPClient http;
    HTTPClient sleepHttp; // New HTTP client for sleep request
    http.setTimeout(HTTP_TIMEOUT);

    // Parse base URL for sleep request
    String baseUrl = imageUrl;
    const char *downloadPath = "/download";
    const char *sleepPath = "/sleep";

    const char *responseHeaders[] = {"X-Photo-Name"};
    http.collectHeaders(responseHeaders, 1);

    String sleepUrl = baseUrl + sleepPath;

    // Setup client for image download
    if (isHttps)
    {
#if EPF_ENABLE_SECURE_HTTP
      secureClient = new WiFiClientSecure;
      secureClient->setInsecure();
      if (!http.begin(*secureClient, imageUrl + downloadPath))
      {
        Serial.println("Failed to initialize HTTPS connection");
        delete secureClient;
        return false;
      }
#endif
    }
    else
    {
      basicClient = new WiFiClient;
      if (!http.begin(*basicClient, imageUrl + downloadPath))
      {
        Serial.println("Failed to initialize HTTP connection");
        delete basicClient;
        return false;
      }
    }

    // Add battery voltage to the request when the selected board exposes a
    // battery monitor. The USB-powered OLED prototype deliberately omits it.
    int batteryVoltage = 0;
#if EPF_HAS_BATTERY_MONITOR
    analogReadResolution(12);
    int plusV = 0;
    for (int i = 0; i < 10; i++)
    {
      plusV += analogReadMilliVolts(BATTERY_ADC_PIN);
      delay(5);
    }
    batteryVoltage = (plusV / 10) * 2;
#endif
    http.addHeader("batteryCap", String(batteryVoltage));

    // Download and process image
    bool success = false;
    requestedSleepSeconds = 0;
    bool retryOnError = true; // Add retry flag

    while (retryOnError && !success)
    {                       // Add retry loop
      retryOnError = false; // Default to no retry

      for (uint8_t i = 0; i < MAX_RETRIES; i++)
      {
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK)
        {
          receivedImageName = http.header("X-Photo-Name");
          success = processImageData(&http);

          // After successful image download, get sleep duration
          if (success)
          {
            // Setup new client for sleep request
            WiFiClient *sleepBasicClient = nullptr;
#if EPF_ENABLE_SECURE_HTTP
            WiFiClientSecure *sleepSecureClient = nullptr;
#endif

            if (isHttps)
            {
#if EPF_ENABLE_SECURE_HTTP
              sleepSecureClient = new WiFiClientSecure;
              sleepSecureClient->setInsecure();
              sleepHttp.begin(*sleepSecureClient, sleepUrl);
#endif
            }
            else
            {
              sleepBasicClient = new WiFiClient;
              sleepHttp.begin(*sleepBasicClient, sleepUrl);
            }

            sleepHttp.addHeader("Accept", "application/json");
            int sleepHttpCode = sleepHttp.GET();

            if (sleepHttpCode == HTTP_CODE_OK)
            {
              String payload = sleepHttp.getString();
              StaticJsonDocument<200> doc;
              DeserializationError error = deserializeJson(doc, payload);

              if (!error)
              {
                requestedSleepSeconds = doc["sleep_duration"] | 0;
                if (requestedSleepSeconds > 0)
                {
                  requestedSleepSeconds /= 1000; // Convert to seconds
                }
              }
            }

            sleepHttp.end();
#if EPF_ENABLE_SECURE_HTTP
            if (sleepSecureClient)
              delete sleepSecureClient;
#endif
            if (sleepBasicClient)
              delete sleepBasicClient;
          }
          break;
        }
        else if (httpCode == HTTP_CODE_ACCEPTED)
        {
          Serial.println("Server processing, waiting...");
          delay(RETRY_DELAY);
        }
        else if (httpCode == HTTP_CODE_INTERNAL_SERVER_ERROR)
        {
          Serial.println("Server error (500), will retry once...");
          delay(RETRY_DELAY);
          retryOnError = true; // Enable one retry on 500 error
          break;               // Exit current retry loop
        }
        else
        {
          Serial.printf("%s GET failed: %s\n",
                        isHttps ? "HTTPS" : "HTTP",
                        http.errorToString(httpCode).c_str());
          break;
        }
      }
    }

    http.end();
    delay(10);
#if EPF_ENABLE_SECURE_HTTP
    if (secureClient)
      delete secureClient;
#endif
    if (basicClient)
      delete basicClient;

    showStatus(success ? "Image received" : "Image fetch failed",
               success
                   ? (receivedImageName.length() > 0 ? receivedImageName : "Endpoint OK")
                   : "Check server");
    return success;
  }

#if EPF_ENABLE_TAILSCALE
  bool connectTailnet()
  {
    if (strlen(TAILSCALE_AUTH_KEY) == 0)
    {
      showStatus("Tailscale key", "Add secrets file");
      Serial.println(F("Tailscale is not configured. Add Arduino/tailscale_secrets.h."));
      return false;
    }

    microlink_config_t config = {};
    config.auth_key = TAILSCALE_AUTH_KEY;
    config.device_name = TAILSCALE_DEVICE_NAME;
    config.enable_derp = true;
    config.enable_stun = true;
    config.enable_disco = true;
    config.max_peers = 8;
    config.wifi_tx_power_dbm = 0;

    showStatus("Connecting Tailscale", TAILSCALE_DEVICE_NAME);
    tailnet = microlink_init(&config);
    if (!tailnet)
    {
      showStatus("Tailscale failed", "Init error");
      return false;
    }

    microlink_set_state_callback(tailnet, onTailscaleState, nullptr);
    if (microlink_start(tailnet) != ESP_OK)
    {
      showStatus("Tailscale failed", "Start error");
      microlink_destroy(tailnet);
      tailnet = nullptr;
      return false;
    }

    const unsigned long deadline = millis() + TAILSCALE_CONNECT_TIMEOUT_MS;
    while (!microlink_is_connected(tailnet) && millis() < deadline)
    {
      delay(250);
    }

    if (!microlink_is_connected(tailnet))
    {
      showStatus("Tailscale failed", "Check auth key");
      Serial.println(F("Tailscale did not reach the connected state before timeout."));
      microlink_stop(tailnet);
      microlink_destroy(tailnet);
      tailnet = nullptr;
      return false;
    }

    char vpnIp[16] = {};
    microlink_ip_to_str(microlink_get_vpn_ip(tailnet), vpnIp);
    tailnetReady = true;
    showStatus("Tailscale ready", vpnIp);
    return true;
  }
#endif

  // check if https
  bool startsWith(const String &str, const char *prefix)
  {
    return str.substring(0, strlen(prefix)).equalsIgnoreCase(prefix);
  }

  // Checks if character is a valid delimiter in image data
  bool isDelimiter(char c)
  {
    return c == ',' || c == '\n' || c == '\r' || c == '\0';
  }

  // Process image data stream and update display
  bool processImageData(HTTPClient *http)
  {
    WiFiClient *stream = http->getStreamPtr();
    int contentLength = http->getSize();

    // Validate content length
    if (contentLength <= 0)
    {
      Serial.println("Invalid content length");
      return false;
    }
    Serial.printf("Content-Length: %d bytes\n", contentLength);
    Serial.println("Starting direct image processing...");

#if EPF_USE_EPAPER
    epd.SendCommand(0x10);
#endif

    const size_t bufferSize = EPF_USE_OLED ? 512U : BUFFER_SIZE;
    uint8_t *buffer = (uint8_t *)malloc(bufferSize);
    if (buffer == NULL)
    {
      Serial.println("Buffer allocation failed");
      return false;
    }

    String hexBuffer;
    int totalBytesProcessed = 0;

    while (contentLength > 0 && http->connected())
    {
      int bytesToRead = min(contentLength, (int)bufferSize);
      int bytesRead = stream->readBytes(buffer, bytesToRead);

      if (bytesRead > 0)
      {
        for (int i = 0; i < bytesRead; i++)
        {
          char c = (char)buffer[i];
          if (isDelimiter(c))
          {
            if (!hexBuffer.isEmpty())
            {
              uint8_t byteValue = (uint8_t)strtol(hexBuffer.c_str(), nullptr, 16);
#if EPF_USE_EPAPER
              epd.SendData(byteValue);
#endif
              hexBuffer.clear();
            }
          }
          else
          {
            hexBuffer += c;
          }
        }

        totalBytesProcessed += bytesRead;
        contentLength -= bytesRead;
      }
      else
      {
        if (!http->connected())
        {
          Serial.println("HTTP connection lost!");
          free(buffer);
          return false;
        }
        delay(10);
      }
    }

    if (!hexBuffer.isEmpty())
    {
      uint8_t byteValue = (uint8_t)strtol(hexBuffer.c_str(), nullptr, 16);
#if EPF_USE_EPAPER
      epd.SendData(byteValue);
#endif
    }

    free(buffer);
    Serial.println("Image data received");
#if EPF_USE_EPAPER
    epd.TurnOnDisplay();
    epd.Sleep();
#endif

    return true;
  }

  // Enter deep sleep mode with calculated wake-up interval
  void hibernate(int sleepDuration = 0)
  {
    Serial.println("Preparing for deep sleep...");

#if EPF_ENABLE_TAILSCALE
    if (tailnet)
    {
      microlink_stop(tailnet);
      microlink_destroy(tailnet);
      tailnet = nullptr;
      tailnetReady = false;
    }
#endif

    // Use provided sleep duration or get default from WiFi manager
    // int sleep_interval = sleepDuration > 0 ? sleepDuration : wifiManager.getServerSleepDuration();
    int sleep_interval = sleepDuration > 0 ? sleepDuration : 86400;

    // Disconnect WiFi and turn off radio
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    fs_deinit();
    delay(50);
    // Print sleep duration for debugging
    Serial.printf("Sleep interval: %d seconds\n", sleep_interval);

    // Convert sleep time to microseconds
    uint64_t sleep_time;
    if (sleep_interval > 0)
    {
      sleep_time = static_cast<uint64_t>(sleep_interval) * 1000000ULL;
    }
    else
    {
      sleep_time = static_cast<uint64_t>(SLEEP_INTERVAL) * 1000000ULL;
    }

    Serial.printf("Sleep time in microseconds: %llu\n", sleep_time);

    // Configure wake up sources
    esp_sleep_enable_timer_wakeup(sleep_time);

    // Configure GPIO wake up
    rtc_gpio_init(WAKEUP_PIN);
    rtc_gpio_set_direction(WAKEUP_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(WAKEUP_PIN);
    rtc_gpio_pulldown_dis(WAKEUP_PIN);
    esp_sleep_enable_ext1_wakeup(1ULL << WAKEUP_PIN, EPF_EXT1_WAKEUP_MODE);

    // Wait for serial output to complete
    Serial.println("Entering deep sleep mode...");
    Serial.flush();

    // Add delay before sleep
    delay(50);

    // Enter deep sleep
    esp_deep_sleep_start();
  }

  static void resetDeviceCredentials(void)
  {
    WifiCaptivePortal.resetSettings();
    bool res = preferences.clear();
    preferences.end();
    ESP.restart();
  }

  // Check if configuration mode should be entered
  bool shouldEnterConfigMode()
  {
#if !EPF_HAS_CONFIG_BUTTON
    return false;
#else
    // Check configuration pin with debounce
    // if (digitalRead(CONFIG_PIN) == LOW) {
    //   delay(BUTTON_DEBOUNCE);
    //   return digitalRead(CONFIG_PIN) == LOW;
    // }
    // return false;
    Button button(CONFIG_PIN);
    return button.result();
#endif
  }

public:
  bool begin()
  {
    Serial.begin(115200);
    delay(50);
    Serial.println(F("Starting EPF hardware profile"));

#if EPF_USE_OLED
    if (!oled.begin())
    {
      Serial.println(F("OLED init failed"));
      return false;
    }
    showStatus("EPF prototype", "OLED ready");
#else
    pinMode(CONFIG_PIN, INPUT_PULLUP);

    if (epd.Init() != 0)
    {
      Serial.println(F("e-Paper init failed"));
      return false;
    }
    Serial.println(F("e-Paper initialized successfully"));
#endif

#if EPF_USE_EPAPER
    // initialize spiffs
    fs_init();
#endif

    // initialize preferences
    preferences.begin("data", false);

    WiFi.mode(WIFI_STA);

    // Check configuration button
    if (shouldEnterConfigMode())
    {
      Serial.println(F("Config button pressed, entering config mode..."));
      showStatus("WiFi setup", "Open frame AP");
#if EPF_USE_EPAPER
      epd.Clear(EPD_7IN3E_WHITE);
#endif
      // epd.Sleep();

      bool res = WifiCaptivePortal.startPortal();
      if (res)
      {
        Serial.println(F("Config mode completed"));
#if EPF_ENABLE_TAILSCALE
        connectTailnet();
#endif
        return true;
      }
      // else {
      //   epd.Clear(EPD_7IN3E_WHITE);
      //   epd.Sleep();
      //   return false;
      // }
    }

    // If button not pressed, try normal startup
    if (WifiCaptivePortal.isSaved())
    {
      showStatus("Connecting WiFi", "Saved network");
      int connection_res = WifiCaptivePortal.autoConnect();
      if (connection_res)
      {
        showStatus("WiFi connected", WiFi.localIP().toString());
        preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);
#if EPF_ENABLE_TAILSCALE
        connectTailnet();
#endif
        return true;
      }
      // else {
      //   epd.Clear(EPD_7IN3E_WHITE);
      //   epd.Sleep();
      // }
    }
    else
    {
      showStatus("WiFi setup", "Open frame AP");
      WifiCaptivePortal.setResetSettingsCallback(resetDeviceCredentials);
      bool res = WifiCaptivePortal.startPortal();
      if (res)
      {
        preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);
#if EPF_ENABLE_TAILSCALE
        connectTailnet();
#endif
        return true;
      }
      //   if (!res) {
      //     epd.Clear(EPD_7IN3E_WHITE);
      //     epd.Sleep();
    }
    // }
    Serial.println(F("No valid WiFi configuration found - main"));
    return false;
  }

  void update()
  {
    Serial.println(F("Update method called"));

    if (WiFi.status() == WL_CONNECTED
#if EPF_ENABLE_TAILSCALE
        && tailnetReady
#endif
    )
    {
      Serial.println(F("WiFi Connected. Downloading image"));
      if (downloadImage())
      {
        Serial.println(F("Image download successful"));
      }
      else
      {
        Serial.println(F("Image download failed"));
      }
    }
    else
    {
#if EPF_ENABLE_TAILSCALE
      showStatus("Tailscale offline", "Retry after reset");
      Serial.println(F("Tailscale is not connected. Cannot download private image"));
#else
      showStatus("WiFi offline", "Retry after reset");
      Serial.println(F("WiFi not connected. Cannot download image"));
#endif
    }

    Serial.println(F("Update completed"));
#if EPF_ENABLE_DEEP_SLEEP
    Serial.println(F("Entering sleep mode"));
    hibernate(requestedSleepSeconds);
#else
    showStatus("Prototype ready", "Logic path complete");
#endif
  }

  // Check battery voltage level
  bool checkVoltage()
  {
#if !EPF_HAS_BATTERY_MONITOR
    Serial.println(F("Battery check skipped for USB OLED prototype"));
    return true;
#else
#if BATTERY_ADC_ENABLE_PIN >= 0
    pinMode(BATTERY_ADC_ENABLE_PIN, OUTPUT);
    digitalWrite(BATTERY_ADC_ENABLE_PIN, HIGH);
#endif
    analogReadResolution(12);
    int analogVolts = analogReadMilliVolts(BATTERY_ADC_PIN);
    // Multiply by 2 due to voltage divider
    Serial.print("BAT millivolts value = ");
    Serial.print(analogVolts * 2);
    Serial.println("mV");
    delay(50);
    // Return false if battery voltage is below 3.05V
    if (analogVolts * 2 < 3050)
    {
      return false;
    }
    return true;
#endif
  }

  // Clear the e-paper display
  void clearScreen()
  {
#if EPF_USE_EPAPER
    epd.Init();
    delay(1000);
    epd.Clear(EPD_7IN3E_WHITE);
    epd.Sleep();
#else
    showStatus("Display clear", "OLED status mode");
#endif
  }
};

// Global instance
EpaperManager epaperManager;

void setup()
{
  // Determine wake up reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER)
  {
    Serial.println("Wakeup caused by timer");
  }
  else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1)
  {
    Serial.println("Wakeup caused by external signal using RTC_GPIO");
  }
  else
  {
    Serial.println("First boot or reset");
  }

  if (!epaperManager.checkVoltage())
  {
    Serial.println(F("Battery low voltage (< 3.0V)"));
    Serial.println(F("Sleep for 24hr"));
    epaperManager.clearScreen();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(1000);
#if EPF_ENABLE_DEEP_SLEEP
    esp_sleep_enable_timer_wakeup(86400 * 1000000ULL);
    esp_deep_sleep_start();
#else
    return;
#endif
  }
  if (epaperManager.begin())
  {
    Serial.println(F("Begin successful, calling update"));
    epaperManager.update();
  }
  else
  {
    Serial.println(F("Begin failed"));
    epaperManager.clearScreen();

    delay(30000);
    ESP.restart();
  }
}

void loop()
{
  // deepsleep
}
