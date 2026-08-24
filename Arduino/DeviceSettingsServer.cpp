#include "DeviceSettingsServer.h"

#include <ArduinoJson.h>
#include <ESP.h>
#include <Preferences.h>
#include <WiFi.h>

#include "config.h"
#include "device_settings_page.h"

namespace
{
constexpr const char *DATA_NAMESPACE = "data";
constexpr const char *WIFI_NAMESPACE = "wificaptive";
constexpr const char *WIFI_LAST_INDEX = "wifi_last_index";
constexpr uint8_t WIFI_MAX_SAVED_CREDS = 5;

String wifiSsidKey(uint8_t index) { return "wifi_" + String(index) + "_ssid"; }
String wifiPasswordKey(uint8_t index) { return "wifi_" + String(index) + "_pswd"; }

bool isSecretPlaceholder(const String &value)
{
    return value.length() == 0 || value == "********" || value == "masked";
}
}

DeviceSettingsServer EpfSettingsServer;

bool DeviceSettingsServer::begin(bool captiveMode)
{
    if (_server)
        end();

    _captive = captiveMode;
    _server = new AsyncWebServer(80);
    if (!_server)
        return false;

    installRoutes();
    return true;
}

void DeviceSettingsServer::start()
{
    if (_server && !_running)
    {
        _server->begin();
        _running = true;
    }
}

void DeviceSettingsServer::end()
{
    if (_server)
    {
        _server->end();
        delete _server;
        _server = nullptr;
    }
    _running = false;
}

void DeviceSettingsServer::setTailscaleStatus(bool connected, const String &ip)
{
    _tailscaleConnected = connected;
    _tailscaleIp = ip;
}

void DeviceSettingsServer::setLastImageResult(bool success, int httpCode)
{
    _lastImageSuccess = success;
    _lastImageHttpCode = httpCode;
}

bool DeviceSettingsServer::authorized(AsyncWebServerRequest *request, bool allowBootstrap) const
{
    Preferences preferences;
    preferences.begin(DATA_NAMESPACE, true);
    String password = preferences.getString(PREFERENCES_ADMIN_PASSWORD, "");
    preferences.end();

    if (password.length() == 0 && allowBootstrap)
        return true;
    if (password.length() == 0)
    {
        request->requestAuthentication();
        return false;
    }

    if (!request->authenticate("admin", password.c_str()))
    {
        request->requestAuthentication();
        return false;
    }
    return true;
}

bool DeviceSettingsServer::hasAdminPassword() const
{
    Preferences preferences;
    preferences.begin(DATA_NAMESPACE, true);
    bool configured = preferences.getString(PREFERENCES_ADMIN_PASSWORD, "").length() > 0;
    preferences.end();
    return configured;
}

bool DeviceSettingsServer::isValidServerUrl(const String &url)
{
    if (url.length() < 10 || url.length() > 160)
        return false;
    if (!(url.startsWith("http://") || url.startsWith("https://")))
        return false;
    if (url.indexOf('@') >= 0 || url.indexOf('?') >= 0 || url.indexOf('#') >= 0)
        return false;

    int schemeEnd = url.indexOf("://");
    int hostStart = schemeEnd + 3;
    int pathStart = url.indexOf('/', hostStart);
    String authority = pathStart >= 0 ? url.substring(hostStart, pathStart) : url.substring(hostStart);
    if (authority.length() == 0 || authority.length() > 100)
        return false;

    int colon = authority.lastIndexOf(':');
    String host = colon > 0 ? authority.substring(0, colon) : authority;
    if (host.length() == 0)
        return false;
    for (size_t i = 0; i < host.length(); ++i)
    {
        char c = host[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']'))
            return false;
    }
    // The EPF application endpoint is intentionally fixed to port 15001.
    // Restricting this here keeps the device from being pointed at arbitrary
    // services on a trusted LAN or tailnet.
    if (colon <= 0)
        return false;
    String port = authority.substring(colon + 1);
    return port == "15001";
}

void DeviceSettingsServer::saveWifiCredentials(const String &ssid, const String &password)
{
    if (ssid.length() == 0)
        return;

    Preferences preferences;
    preferences.begin(WIFI_NAMESPACE, false);
    String oldSsids[WIFI_MAX_SAVED_CREDS];
    String oldPasswords[WIFI_MAX_SAVED_CREDS];
    for (uint8_t i = 0; i < WIFI_MAX_SAVED_CREDS; ++i)
    {
        oldSsids[i] = preferences.getString(wifiSsidKey(i).c_str(), "");
        oldPasswords[i] = preferences.getString(wifiPasswordKey(i).c_str(), "");
    }
    String savedPassword = password;
    if (isSecretPlaceholder(password))
    {
        for (uint8_t i = 0; i < WIFI_MAX_SAVED_CREDS; ++i)
        {
            if (oldSsids[i] == ssid)
            {
                savedPassword = oldPasswords[i];
                break;
            }
        }
    }
    for (int i = WIFI_MAX_SAVED_CREDS - 1; i > 0; --i)
    {
        preferences.putString(wifiSsidKey(i).c_str(), oldSsids[i - 1]);
        preferences.putString(wifiPasswordKey(i).c_str(), oldPasswords[i - 1]);
    }
    preferences.putString(wifiSsidKey(0).c_str(), ssid);
    preferences.putString(wifiPasswordKey(0).c_str(), savedPassword);
    preferences.putInt(WIFI_LAST_INDEX, 0);
    preferences.end();
}

String DeviceSettingsServer::settingsJson() const
{
    Preferences preferences;
    preferences.begin(DATA_NAMESPACE, true);
    String serverUrl = preferences.getString("SERVER_BASE_URL", SERVER_BASE_URL);
    bool tailscaleEnabled = preferences.getBool(PREFERENCES_TAILSCALE_ENABLED, EPF_DEFAULT_TAILSCALE);
    String tailscaleName = preferences.getString(PREFERENCES_TAILSCALE_NAME, TAILSCALE_DEVICE_NAME);
    bool hasAdminPassword = preferences.getString(PREFERENCES_ADMIN_PASSWORD, "").length() > 0;
    uint32_t refreshRate = preferences.getUInt(PREFERENCES_REFRESH_RATE, SLEEP_INTERVAL);
    preferences.end();

    Preferences wifi;
    wifi.begin(WIFI_NAMESPACE, true);
    String ssid = wifi.getString(wifiSsidKey(0).c_str(), "");
    wifi.end();

    StaticJsonDocument<512> doc;
    doc["server_url"] = serverUrl;
    doc["captive"] = _captive;
    doc["wifi_ssid"] = ssid;
    doc["tailscale_enabled"] = tailscaleEnabled;
    doc["tailscale_name"] = tailscaleName;
    doc["tailscale_compiled"] = EPF_ENABLE_TAILSCALE != 0;
    doc["has_admin_password"] = hasAdminPassword;
    doc["refresh_rate"] = refreshRate;
    String output;
    serializeJson(doc, output);
    return output;
}

String DeviceSettingsServer::statusJson() const
{
    Preferences preferences;
    preferences.begin(DATA_NAMESPACE, true);
    String serverUrl = preferences.getString("SERVER_BASE_URL", SERVER_BASE_URL);
    preferences.end();

    StaticJsonDocument<512> doc;
    doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
    doc["wifi_ip"] = WiFi.localIP().toString();
    doc["tailscale_enabled"] = [&]() {
        Preferences p;
        p.begin(DATA_NAMESPACE, true);
        bool value = p.getBool(PREFERENCES_TAILSCALE_ENABLED, EPF_DEFAULT_TAILSCALE);
        p.end();
        return value;
    }();
    doc["tailscale_compiled"] = EPF_ENABLE_TAILSCALE != 0;
    doc["tailscale_connected"] = _tailscaleConnected;
    doc["tailscale_ip"] = _tailscaleIp;
    doc["heap"] = ESP.getFreeHeap();
    doc["firmware"] = EPF_FIRMWARE_VERSION;
    doc["server_url"] = serverUrl;
    doc["captive"] = _captive;
    doc["last_image_success"] = _lastImageSuccess;
    doc["last_image_http_status"] = _lastImageHttpCode;
    String output;
    serializeJson(doc, output);
    return output;
}

void DeviceSettingsServer::installRoutes()
{
    if (!_server)
        return;

    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!_captive && !authorized(request, false))
            return;
        request->send_P(200, "text/html", hasAdminPassword() ? DEVICE_SETTINGS_PAGE : DEVICE_ADMIN_SETUP_PAGE);
    });

    auto bootstrapHandler = new AsyncCallbackJsonWebHandler("/api/bootstrap", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!_captive || hasAdminPassword())
        {
            request->send(409, "application/json", "{\"error\":\"admin password already configured\"}");
            return;
        }

        String password = json["admin_password"] | "";
        if (password.length() < 12 || password.length() > 95)
        {
            request->send(400, "application/json", "{\"error\":\"admin password must be 8 to 95 characters\"}");
            return;
        }

        Preferences preferences;
        preferences.begin(DATA_NAMESPACE, false);
        preferences.putString(PREFERENCES_ADMIN_PASSWORD, password);
        preferences.end();
        request->send(200, "application/json", "{\"saved\":true}");
    });
    _server->addHandler(bootstrapHandler);

    _server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!authorized(request, false)) return;
        request->send(200, "application/json", statusJson());
    });

    _server->on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!authorized(request, false)) return;
        request->send(200, "application/json", settingsJson());
    });

    auto settingsHandler = new AsyncCallbackJsonWebHandler("/api/settings", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!authorized(request, false))
            return;

        JsonObject data = json.as<JsonObject>();
        Preferences preferences;
        preferences.begin(DATA_NAMESPACE, false);

        String url = data["server_url"] | "";
        if (url.length() > 0 && !isValidServerUrl(url))
        {
            preferences.end();
            request->send(400, "application/json", "{\"error\":\"invalid server_url\"}");
            return;
        }
        if (url.length() > 0)
            preferences.putString("SERVER_BASE_URL", url);

        bool restartRequired = false;
        if (data.containsKey("tailscale_enabled"))
        {
            bool enabled = data["tailscale_enabled"] | false;
            if (enabled && !EPF_ENABLE_TAILSCALE)
            {
                preferences.end();
                request->send(400, "application/json", "{\"error\":\"Tailscale support is not included in this firmware\"}");
                return;
            }
            if (preferences.getBool(PREFERENCES_TAILSCALE_ENABLED, EPF_DEFAULT_TAILSCALE) != enabled)
                restartRequired = true;
            preferences.putBool(PREFERENCES_TAILSCALE_ENABLED, enabled);
        }

        String name = data["tailscale_name"] | "";
        if (name.length() > 0 && name.length() <= 47)
        {
            String oldName = preferences.getString(PREFERENCES_TAILSCALE_NAME, TAILSCALE_DEVICE_NAME);
            if (oldName != name)
                restartRequired = true;
            preferences.putString(PREFERENCES_TAILSCALE_NAME, name);
        }

        String adminPassword = data["admin_password"] | "";
        if (!isSecretPlaceholder(adminPassword))
            preferences.putString(PREFERENCES_ADMIN_PASSWORD, adminPassword);

        String tailscaleKey = data["tailscale_auth_key"] | "";
        if (!isSecretPlaceholder(tailscaleKey))
        {
            if (!tailscaleKey.startsWith("tskey-") || tailscaleKey.length() > 95)
            {
                preferences.end();
                request->send(400, "application/json", "{\"error\":\"invalid Tailscale auth key\"}");
                return;
            }
            preferences.putString(PREFERENCES_TAILSCALE_AUTH_KEY, tailscaleKey);
            restartRequired = true;
        }

        if (data.containsKey("refresh_rate"))
        {
            uint32_t refreshRate = data["refresh_rate"] | SLEEP_INTERVAL;
            if (refreshRate >= MIN_SLEEP_TIME)
                preferences.putUInt(PREFERENCES_REFRESH_RATE, refreshRate);
        }
        preferences.end();

        String ssid = data["wifi_ssid"] | "";
        String wifiPassword = data["wifi_password"] | "";
        bool wifiChanged = ssid.length() > 0;
        if (wifiChanged)
        {
            saveWifiCredentials(ssid, wifiPassword);
            restartRequired = true;
        }

        StaticJsonDocument<160> result;
        result["saved"] = true;
        result["restart_required"] = restartRequired;
        String output;
        serializeJson(result, output);
        request->send(200, "application/json", output);
    });
    _server->addHandler(settingsHandler);

    _server->on("/api/restart", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!authorized(request, false))
            return;
        request->send(200, "application/json", "{\"restarting\":true}");
        if (_restartCallback)
        {
            delay(250);
            _restartCallback();
        }
    });

    _server->on("/api/factory-reset", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!authorized(request, false))
            return;
        request->send(200, "application/json", "{\"resetting\":true}");
        if (_factoryResetCallback)
        {
            delay(250);
            _factoryResetCallback();
        }
    });
}
