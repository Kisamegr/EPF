#ifndef DEVICE_SETTINGS_SERVER_H
#define DEVICE_SETTINGS_SERVER_H

#include <Arduino.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <functional>

class DeviceSettingsServer
{
private:
    AsyncWebServer *_server = nullptr;
    bool _captive = false;
    bool _running = false;
    bool _tailscaleConnected = false;
    String _tailscaleIp;
    bool _lastImageSuccess = false;
    int _lastImageHttpCode = 0;
    std::function<void()> _restartCallback;
    std::function<void()> _factoryResetCallback;

    void installRoutes();
    bool authorized(AsyncWebServerRequest *request, bool allowBootstrap) const;
    bool hasAdminPassword() const;
    void saveWifiCredentials(const String &ssid, const String &password);
    String settingsJson() const;
    String statusJson() const;

public:
    bool begin(bool captiveMode = false);
    void start();
    void end();
    AsyncWebServer *server() const { return _server; }
    bool isRunning() const { return _running; }

    void setRestartCallback(std::function<void()> callback) { _restartCallback = callback; }
    void setFactoryResetCallback(std::function<void()> callback) { _factoryResetCallback = callback; }
    void setTailscaleStatus(bool connected, const String &ip);
    void setLastImageResult(bool success, int httpCode);

    static bool isValidServerUrl(const String &url);
};

extern DeviceSettingsServer EpfSettingsServer;

#endif
