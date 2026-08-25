#include <Arduino.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <Update.h>
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
#include <SPIFFS.h>
#include "mbedtls/sha256.h"
#include <WifiCaptive.h>
#include <DeviceSettingsServer.h>
#include <filesystem.h>
#if EPF_USE_OLED
#include "oled_status.h"
#endif

Preferences preferences;

// Shared IO buffer reused by OTA firmware download and panel image streaming.
// Both operations are mutually exclusive within a single boot cycle: when an
// OTA update is pending the device downloads + installs firmware and restarts
// before ever touching the image path, so a single static region is safe.
static uint8_t s_io_buffer[BUFFER_SIZE];

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
  String tailscaleAuthKey = "";
  String tailscaleDeviceName = "";
  int requestedSleepSeconds = 0;
  bool tailnetReady = false;
  bool tailscaleEnabled = false;

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

#if EPF_USE_EPAPER
  static int glyphIndex(char character)
  {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= '2' && character <= '9') return 26 + character - '2';
    if (character == '-') return 34;
    if (character == ':') return 35;
    return -1;
  }

  static uint8_t glyphRow(char character, uint8_t row)
  {
    // Five-by-seven glyphs for the intentionally small provisioning screen.
    static const uint8_t glyphs[][7] = {
      {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{15,16,16,16,16,16,15},
      {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
      {15,16,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
      {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
      {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
      {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
      {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
      {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
      {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},{14,17,19,21,25,17,14},
      {4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
      {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},
      {31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
      {0,0,0,31,0,0,0},{0,4,0,0,4,0,0}
    };
    int index = glyphIndex(character);
    return index < 0 || row >= 7 ? 0 : glyphs[index][row];
  }

  static bool textPixel(const String &text, int originX, int originY, int scale, int x, int y)
  {
    if (y < originY || y >= originY + 7 * scale || x < originX) return false;
    int character = (x - originX) / (6 * scale);
    if (character < 0 || character >= static_cast<int>(text.length())) return false;
    int column = ((x - originX) % (6 * scale)) / scale;
    if (column >= 5) return false;
    uint8_t row = (y - originY) / scale;
    return (glyphRow(text[character], row) & (1 << (4 - column))) != 0;
  }

  void showProvisioningScreen()
  {
    const String password = WifiCaptive::provisioningPassword();
    const String heading = "EPF SETUP";
    const String ssid = "SSID: EPF-SETUP";
    epd.SendCommand(0x10);
    for (int y = 0; y < EPD_HEIGHT; ++y)
    {
      for (int x = 0; x < EPD_WIDTH; x += 2)
      {
        bool left = textPixel(heading, 180, 70, 12, x, y) || textPixel(ssid, 100, 220, 7, x, y) || textPixel(password, 40, 320, 6, x, y);
        bool right = textPixel(heading, 180, 70, 12, x + 1, y) || textPixel(ssid, 100, 220, 7, x + 1, y) || textPixel(password, 40, 320, 6, x + 1, y);
        epd.SendData((left ? EPD_7IN3E_BLACK : EPD_7IN3E_WHITE) << 4 | (right ? EPD_7IN3E_BLACK : EPD_7IN3E_WHITE));
      }
    }
    epd.TurnOnDisplay();
    Serial.printf("Provisioning SSID: %s; password: %s\n", WIFI_SSID, password.c_str());
  }
#else
  void showProvisioningScreen()
  {
    showStatus("AP password", WifiCaptive::provisioningPassword());
  }
#endif

  void sendOtaAck(bool success, const String &errorDetail)
  {
    WiFiClient ackBasic;
#if EPF_ENABLE_SECURE_HTTP
    WiFiClientSecure ackSecure;
#endif
    HTTPClient ackHttp;
    bool ackReady = false;
    bool isHttps = imageUrl.startsWith("https://");

    if (isHttps)
    {
#if EPF_ENABLE_SECURE_HTTP
#ifdef EPF_HAS_SERVER_CA_CERT
      ackSecure.setCACert(EpfServerCaCert);
      ackReady = ackHttp.begin(ackSecure, imageUrl + "/ota/ack");
#endif
#endif
    }
    else
    {
      ackReady = ackHttp.begin(ackBasic, imageUrl + "/ota/ack");
    }

    if (ackReady)
    {
      ackHttp.addHeader("Authorization", String("Bearer ") + EPF_DEVICE_TOKEN);
      ackHttp.addHeader("Content-Type", "application/json");
      String jsonBody = "{\"status\":\"" + String(success ? "success" : "failed") + "\",\"error\":\"" + errorDetail + "\"}";
      ackHttp.POST(jsonBody);
      ackHttp.end();
    }
  }

  bool performOtaUpdate()
  {
    showStatus("OTA Update", "Downloading FW...");
    Serial.println(F("[OTA] Pending OTA update detected. Starting firmware update..."));

    WiFiClient basicClient;
#if EPF_ENABLE_SECURE_HTTP
    WiFiClientSecure secureClient;
#endif
    HTTPClient http;
    http.setTimeout(30000);

    const char *otaBinaryPath = "/ota/binary";
    const char *responseHeaders[] = {"X-EPF-OTA-SHA256", "Content-Length"};
    http.collectHeaders(responseHeaders, 2);

    bool isHttps = imageUrl.startsWith("https://");
    bool initOk = false;

    if (isHttps)
    {
#if EPF_ENABLE_SECURE_HTTP
#ifdef EPF_HAS_SERVER_CA_CERT
      secureClient.setCACert(EpfServerCaCert);
      initOk = http.begin(secureClient, imageUrl + otaBinaryPath);
#endif
#endif
    }
    else
    {
      initOk = http.begin(basicClient, imageUrl + otaBinaryPath);
    }

    if (!initOk)
    {
      Serial.println(F("[OTA] Failed to initialize connection to /ota/binary"));
      sendOtaAck(false, "Connection init failed");
      return false;
    }

    http.addHeader("Authorization", String("Bearer ") + EPF_DEVICE_TOKEN);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
      Serial.printf("[OTA] GET /ota/binary failed with HTTP code %d\n", httpCode);
      String err = "HTTP " + String(httpCode);
      http.end();
      sendOtaAck(false, err);
      return false;
    }

    int contentLength = http.getSize();
    String expectedSha256 = http.header("X-EPF-OTA-SHA256");

    if (contentLength <= 0)
    {
      Serial.println(F("[OTA] Invalid Content-Length header"));
      http.end();
      sendOtaAck(false, "Invalid Content-Length");
      return false;
    }

    Serial.printf("[OTA] Firmware binary size: %d bytes, Expected SHA256: %s\n", contentLength, expectedSha256.c_str());

    if (!Update.begin(contentLength))
    {
      Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
      String err = String("Update.begin failed: ") + Update.errorString();
      http.end();
      sendOtaAck(false, err);
      return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t *buffer = s_io_buffer;
    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);

    size_t written = 0;
    unsigned long lastData = millis();
    bool writeFailed = false;

    while (written < static_cast<size_t>(contentLength) && millis() - lastData < 60000)
    {
      int availableBytes = stream->available();
      if (availableBytes > 0)
      {
        int bytesRead = stream->readBytes(buffer, min(static_cast<size_t>(availableBytes), static_cast<size_t>(BUFFER_SIZE)));
        if (bytesRead > 0)
        {
          size_t bytesWritten = Update.write(buffer, bytesRead);
          if (bytesWritten != static_cast<size_t>(bytesRead))
          {
            Serial.printf("[OTA] Update.write failed! Read %d, wrote %d: %s\n", bytesRead, bytesWritten, Update.errorString());
            writeFailed = true;
            break;
          }
          mbedtls_sha256_update_ret(&sha, buffer, bytesRead);
          written += bytesRead;
          lastData = millis();
        }
      }
      else
      {
        delay(10);
      }
    }

    http.end();
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (writeFailed || written != static_cast<size_t>(contentLength))
    {
      Serial.printf("[OTA] Download or write incomplete. Written %d / %d bytes\n", written, contentLength);
      Update.abort();
      sendOtaAck(false, "Incomplete download or write error");
      return false;
    }

    char actualSha256[65];
    for (size_t i = 0; i < sizeof(digest); ++i) sprintf(actualSha256 + (i * 2), "%02x", digest[i]);
    actualSha256[64] = '\0';

    if (expectedSha256.length() == 64 && expectedSha256.equalsIgnoreCase(actualSha256) == false)
    {
      Serial.printf("[OTA] SHA-256 mismatch! Expected: %s, Actual: %s\n", expectedSha256.c_str(), actualSha256);
      Update.abort();
      sendOtaAck(false, "SHA-256 integrity check failed");
      return false;
    }

    if (!Update.end(true))
    {
      Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
      String err = String("Update.end failed: ") + Update.errorString();
      sendOtaAck(false, err);
      return false;
    }

    Serial.println(F("[OTA] Firmware binary verified & flashed successfully!"));
    showStatus("OTA Success", "Rebooting into new FW");

    sendOtaAck(true, "");
    delay(500);

    Serial.println(F("[OTA] Restarting ESP32..."));
    ESP.restart();
    return true;
  }

  // Resolve and cache the server base URL from persisted preferences, falling
  // back to the compile-time default. Returns false and shows a status message
  // when no URL is configured at all.
  bool resolveServerUrl()
  {
    imageUrl = preferences.getString("SERVER_BASE_URL", "");
    if (imageUrl.length() == 0)
      imageUrl = SERVER_BASE_URL;
    if (imageUrl.length() == 0)
    {
      showStatus("No server URL", "Configure portal");
      return false;
    }
    return true;
  }

  // GET /ota/check: returns true only when an OTA update was triggered (device
  // will restart). Returns false when no update is pending or on any error, so
  // the caller can safely continue to the image-download path.
  bool checkOtaAndUpdate()
  {
    if (!resolveServerUrl())
      return false;

    Serial.println(F("[OTA] Checking for firmware update..."));
    bool isHttps = imageUrl.startsWith("https://");

    WiFiClient basicClient;
#if EPF_ENABLE_SECURE_HTTP
    WiFiClientSecure secureClient;
#endif
    HTTPClient http;
    http.setTimeout(10000); // short timeout: this is a lightweight JSON probe
    bool initOk = false;

    if (isHttps)
    {
#if EPF_ENABLE_SECURE_HTTP
#ifdef EPF_HAS_SERVER_CA_CERT
      secureClient.setCACert(EpfServerCaCert);
      initOk = http.begin(secureClient, imageUrl + "/ota/check");
#endif
#endif
    }
    else
    {
      initOk = http.begin(basicClient, imageUrl + "/ota/check");
    }

    if (!initOk)
    {
      Serial.println(F("[OTA] Could not open /ota/check — skipping OTA probe"));
      return false;
    }

    http.addHeader("Authorization", String("Bearer ") + EPF_DEVICE_TOKEN);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK)
    {
      Serial.printf("[OTA] /ota/check returned %d — skipping OTA probe\n", httpCode);
      http.end();
      return false;
    }

    // Only extract the "available" boolean. The "staged" field may contain a
    // 64-char SHA-256, filename, size, and timestamp — well over 128 bytes.
    // The filter tells ArduinoJson to skip every key except "available".
    // Both documents need at least JSON_OBJECT_SIZE(1) = 16 bytes; use 64 to
    // be safe against ArduinoJson version differences.
    // Use getString() rather than the stream so the connection is fully drained
    // and closed before we open the /ota/binary connection.
    String body = http.getString();
    http.end();

    StaticJsonDocument<64> filter;
    filter["available"] = true;
    StaticJsonDocument<64> doc;
    DeserializationError err = deserializeJson(doc, body,
                                               DeserializationOption::Filter(filter));

    if (err || !doc["available"].is<bool>())
    {
      Serial.printf("[OTA] /ota/check parse failed (%s) — skipping OTA probe\n", err.c_str());
      return false;
    }

    if (!doc["available"].as<bool>())
    {
      Serial.println(F("[OTA] No firmware update staged."));
      return false;
    }

    Serial.println(F("[OTA] Firmware update available. Starting OTA..."));
    performOtaUpdate(); // restarts on success; falls through on failure
    return false;       // OTA failed; let caller decide what to do next
  }

  bool downloadImage()
  {
    receivedImageName = "";
    if (!resolveServerUrl())
      return false;

    showStatus("Fetching image", imageUrl);
    Serial.print("[EPF] Server URL: ");
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
    // Declare transport clients before HTTPClient so their destructors run
    // after HTTPClient's destructor. HTTPClient keeps a client reference and
    // can otherwise touch a dangling object during automatic cleanup.
    WiFiClient basicClient;
#if EPF_ENABLE_SECURE_HTTP
    WiFiClientSecure secureClient;
#endif
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);

    const char *downloadPath = "/download";

    const char *responseHeaders[] = {"X-Photo-Name", "X-EPF-Protocol", "X-Delivery-Id", "X-Payload-SHA256", "X-Sleep-Seconds"};
    http.collectHeaders(responseHeaders, 5);

    // Setup client for image download
    if (isHttps)
    {
#if EPF_ENABLE_SECURE_HTTP
#ifdef EPF_HAS_SERVER_CA_CERT
      secureClient.setCACert(EpfServerCaCert);
#else
      Serial.println(F("HTTPS certificate is not configured"));
      return false;
#endif
      if (!http.begin(secureClient, imageUrl + downloadPath))
      {
        Serial.println("Failed to initialize HTTPS connection");
        return false;
      }
#endif
    }
    else
    {
#if !EPF_ENABLE_TAILSCALE
      showStatus("HTTPS required", "or use Tailscale");
      return false;
#endif
      if (!http.begin(basicClient, imageUrl + downloadPath))
      {
        Serial.println("Failed to initialize HTTP connection");
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
#elif defined(EPF_PROTOTYPE_FAKE_BATTERY_MV)
    // The USB-powered OLED prototype has no battery ADC. Report a stable
    // representative voltage so the server-side battery indicator can be
    // exercised during integration testing (3808 mV maps to 42%).
    batteryVoltage = EPF_PROTOTYPE_FAKE_BATTERY_MV;
#endif
    http.addHeader("batteryCap", String(batteryVoltage));
    if (String(EPF_DEVICE_TOKEN).length() < 32)
    {
      showStatus("Device token missing", "Reflash securely");
      return false;
    }
    http.addHeader("Authorization", String("Bearer ") + EPF_DEVICE_TOKEN);

    // Download and process image
    bool success = false;
    int lastHttpCode = 0;
    requestedSleepSeconds = 0;
    const unsigned long deadline = millis() + EPF_WAKE_DEADLINE_MS;
    for (uint8_t i = 0; i < MAX_RETRIES && !success && millis() < deadline; i++)
    {
        int httpCode = http.GET();
        lastHttpCode = httpCode;

        if (httpCode == HTTP_CODE_OK)
        {
          receivedImageName = http.header("X-Photo-Name");
          success = processImageData(&http);

          if (success)
          {
            requestedSleepSeconds = http.header("X-Sleep-Seconds").toInt();
            WiFiClient ackBasic;
#if EPF_ENABLE_SECURE_HTTP
            WiFiClientSecure ackSecure;
#endif
            // Keep the client objects alive until ack's destructor runs.
            HTTPClient ack;
            bool ackReady = false;
            if (isHttps)
            {
 #if EPF_ENABLE_SECURE_HTTP
 #ifdef EPF_HAS_SERVER_CA_CERT
              ackSecure.setCACert(EpfServerCaCert);
              ackReady = ack.begin(ackSecure, imageUrl + "/ack");
 #endif
 #endif
            }
            else
            {
              ackReady = ack.begin(ackBasic, imageUrl + "/ack");
            }
            if (ackReady)
            {
              ack.addHeader("Authorization", String("Bearer ") + EPF_DEVICE_TOKEN);
              ack.addHeader("X-Delivery-Id", http.header("X-Delivery-Id"));
              success = ack.POST("") == HTTP_CODE_OK;
              ack.end();
            }
          }
          break;
        }
        else if (httpCode == HTTP_CODE_ACCEPTED || httpCode == 429 || httpCode == 502 || httpCode == 503 || httpCode == 504)
        {
          delay(RETRY_DELAY * (i + 1));
        }
        else
        {
          Serial.printf("%s GET failed: %s\n",
                        isHttps ? "HTTPS" : "HTTP",
                        http.errorToString(httpCode).c_str());
          break;
        }
    }

    http.end();
    delay(10);

    showStatus(success ? "Image received" : "Image fetch failed",
               success
                   ? (receivedImageName.length() > 0 ? receivedImageName : "Endpoint OK")
                   : "Check server");
    EpfSettingsServer.setLastImageResult(success, lastHttpCode);
    return success;
  }

#if EPF_ENABLE_TAILSCALE
  bool connectTailnet()
  {
    Preferences settings;
    settings.begin("data", true);
    tailscaleEnabled = settings.getBool(PREFERENCES_TAILSCALE_ENABLED, EPF_DEFAULT_TAILSCALE);
    tailscaleAuthKey = settings.getString(PREFERENCES_TAILSCALE_AUTH_KEY, "");
    tailscaleDeviceName = settings.getString(PREFERENCES_TAILSCALE_NAME, TAILSCALE_DEVICE_NAME);
    settings.end();

    if (!tailscaleEnabled)
    {
      EpfSettingsServer.setTailscaleStatus(false, "");
      showStatus("Tailscale disabled", "WiFi only");
      return true;
    }

    // Keep compatibility with existing prototype builds that used the
    // compile-time secret, while making the persisted setting the normal path.
    if (tailscaleAuthKey.length() == 0)
      tailscaleAuthKey = TAILSCALE_AUTH_KEY;

    if (tailscaleAuthKey.length() == 0)
    {
      showStatus("Tailscale key", "Add secrets file");
      Serial.println(F("Tailscale is not configured. Add Arduino/tailscale_secrets.h."));
      return false;
    }

    microlink_config_t config = {};
    config.auth_key = tailscaleAuthKey.c_str();
    config.device_name = tailscaleDeviceName.c_str();
    config.enable_derp = true;
    config.enable_stun = true;
    config.enable_disco = true;
    config.max_peers = 8;
    config.wifi_tx_power_dbm = 0;

    showStatus("Connecting Tailscale", tailscaleDeviceName);
    tailnet = microlink_init(&config);
    if (!tailnet)
    {
      showStatus("Tailscale failed", "Init error");
      EpfSettingsServer.setTailscaleStatus(false, "");
      return false;
    }

    microlink_set_state_callback(tailnet, onTailscaleState, nullptr);
    if (microlink_start(tailnet) != ESP_OK)
    {
      showStatus("Tailscale failed", "Start error");
      microlink_destroy(tailnet);
      tailnet = nullptr;
      EpfSettingsServer.setTailscaleStatus(false, "");
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
      EpfSettingsServer.setTailscaleStatus(false, "");
      return false;
    }

    char vpnIp[16] = {};
    microlink_ip_to_str(microlink_get_vpn_ip(tailnet), vpnIp);
    tailnetReady = true;
    EpfSettingsServer.setTailscaleStatus(true, String(vpnIp));
    showStatus("Tailscale ready", vpnIp);
    return true;
  }
#endif

  // check if https
  bool startsWith(const String &str, const char *prefix)
  {
    return str.substring(0, strlen(prefix)).equalsIgnoreCase(prefix);
  }

  // Download a fixed-size binary payload to SPIFFS, verify its SHA-256, and
  // only then drive the panel. This avoids a partial panel update on a truncated
  // or malicious response.
  bool processImageData(HTTPClient *http)
  {
    WiFiClient *stream = http->getStreamPtr();
    int contentLength = http->getSize();
    String checksum = http->header("X-Payload-SHA256");
    if (http->header("X-EPF-Protocol") != "2" || http->header("X-Delivery-Id").length() != 32 ||
        contentLength != EPF_PANEL_PAYLOAD_BYTES || checksum.length() != 64)
    {
      Serial.println("Invalid EPF response metadata");
      return false;
    }
    File staged = SPIFFS.open("/epf-image.bin", FILE_WRITE);
    if (!staged)
    {
      Serial.println("Could not stage panel payload");
      return false;
    }
    uint8_t *buffer = s_io_buffer;
    uint8_t digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    size_t received = 0;
    unsigned long lastData = millis();
    while (received < EPF_PANEL_PAYLOAD_BYTES && millis() - lastData < HTTP_TIMEOUT)
    {
      int bytesRead = stream->readBytes(buffer, min(static_cast<size_t>(BUFFER_SIZE), EPF_PANEL_PAYLOAD_BYTES - received));
      if (bytesRead > 0)
      {
        if (staged.write(buffer, bytesRead) != static_cast<size_t>(bytesRead)) break;
        mbedtls_sha256_update_ret(&sha, buffer, bytesRead);
        received += bytesRead;
        lastData = millis();
      }
      else delay(10);
    }
    staged.close();
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    char actual[65];
    for (size_t i = 0; i < sizeof(digest); ++i) sprintf(actual + (i * 2), "%02x", digest[i]);
    actual[64] = '\0';
    if (received != EPF_PANEL_PAYLOAD_BYTES || checksum != String(actual))
    {
      SPIFFS.remove("/epf-image.bin");
      Serial.println("Panel payload integrity check failed");
      return false;
    }
    staged = SPIFFS.open("/epf-image.bin", FILE_READ);
    if (!staged) return false;
#if EPF_USE_EPAPER
    epd.SendCommand(0x10);
    while (staged.available())
    {
      size_t count = staged.read(buffer, sizeof(buffer));
      for (size_t i = 0; i < count; ++i) epd.SendData(buffer[i]);
    }
    epd.TurnOnDisplay();
    epd.Sleep();
#endif
    staged.close();
    SPIFFS.remove("/epf-image.bin");
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
    int sleep_interval = sleepDuration > 0 ? sleepDuration :
        static_cast<int>(preferences.getUInt(PREFERENCES_REFRESH_RATE, SLEEP_INTERVAL));

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
    preferences.clear();
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

    // Stage the verified binary response in SPIFFS before updating the display.
    if (!fs_init())
      return false;

    // initialize preferences
    preferences.begin("data", false);

    WiFi.mode(WIFI_STA);

    // Check configuration button
    if (shouldEnterConfigMode())
    {
      Serial.println(F("Config button pressed, entering config mode..."));
      showProvisioningScreen();
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
#if !EPF_HAS_CONFIG_BUTTON
      // The prototype has no setup button. Only an unprovisioned unit opens a
      // time-limited portal; an already-provisioned unit never does so after a
      // routine connection failure.
      showProvisioningScreen();
      WifiCaptivePortal.setResetSettingsCallback(resetDeviceCredentials);
      if (WifiCaptivePortal.startPortal())
      {
        preferences.putInt(PREFERENCES_CONNECT_WIFI_RETRY_COUNT, 1);
        return true;
      }
#else
      // Provisioning is physical-presence only. Hold the configuration button
      // before boot rather than exposing an AP automatically on first run.
      showStatus("Hold setup button", "to provision WiFi");
      return false;
#endif
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
        && (!tailscaleEnabled || tailnetReady)
#endif
    )
    {
      // Check for a staged OTA firmware update before touching the image path.
      // /ota/check is a tiny JSON probe; if an update is available the device
      // will download, verify, flash and restart — image fetch happens on the
      // next boot with the new firmware already running.
      if (checkOtaAndUpdate())
      {
        // Should not be reached: performOtaUpdate() calls ESP.restart() on
        // success. We land here only on OTA failure, in which case we still
        // skip the image download so the error is not silently swallowed.
        Serial.println(F("OTA update attempted but did not restart — skipping image fetch."));
      }
      else
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
