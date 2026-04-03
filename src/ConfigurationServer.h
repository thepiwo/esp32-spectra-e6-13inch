#ifndef CONFIGURATION_SERVER_H
#define CONFIGURATION_SERVER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include <functional>

struct Configuration {
  String ssid;
  String password;
  String imageUrl;
  String folderUrl;
  String pinnedImageUrl;
  uint8_t ditherMode = 0;
  uint8_t scalingMode = 0; // Default to FIT (Letterbox)
  uint16_t sleepMinutes = 0;
  uint16_t imageChangeMinutes = 30;
  uint8_t quietHoursStart = 0;
  uint8_t quietHoursEnd   = 0;
  int8_t  utcOffsetHours  = 0;

  Configuration() = default;

  Configuration(const String &ssid, const String &password,
                const String &imageUrl, const String &folderUrl = "",
                const String &pinnedImageUrl = "", uint8_t ditherMode = 0,
                uint8_t scalingMode = 0, uint16_t sleepMinutes = 0,
                uint16_t imageChangeMinutes = 30,
                uint8_t quietHoursStart = 0, uint8_t quietHoursEnd = 0,
                int8_t utcOffsetHours = 0)
      : ssid(ssid), password(password), imageUrl(imageUrl),
        folderUrl(folderUrl), pinnedImageUrl(pinnedImageUrl),
        ditherMode(ditherMode), scalingMode(scalingMode),
        sleepMinutes(sleepMinutes), imageChangeMinutes(imageChangeMinutes),
        quietHoursStart(quietHoursStart), quietHoursEnd(quietHoursEnd),
        utcOffsetHours(utcOffsetHours) {}
};

using OnSaveCallback = std::function<void(const Configuration &config)>;
using OnPinCallback = std::function<void(const String &pinnedImageUrl)>;

class ConfigurationServer {
public:
  static const char *WIFI_AP_NAME;
  static const char *WIFI_AP_PASSWORD;

  ConfigurationServer(const Configuration &currentConfig);
  void run(OnSaveCallback onSaveCallback, bool startAP = true);
  void stop();
  bool isRunning() const;
  void handleRequests();

  String getWifiAccessPointName() const;
  String getWifiAccessPointPassword() const;

  void setOnPinCallback(OnPinCallback callback) { onPinCallback = callback; }

  // Returns true if a new image was uploaded since last check
  bool hasNewImage() const { return newImageUploaded; }
  void clearNewImage() { newImageUploaded = false; }

  // Returns true if the user pressed "Change Image Now"
  bool isRefreshRequested() const { return refreshRequested; }
  void clearRefreshRequest() { refreshRequested = false; }

private:
  String deviceName;
  String wifiAccessPointName;
  String wifiAccessPointPassword;

  Configuration currentConfiguration;

  AsyncWebServer *server;
  DNSServer *dnsServer;
  bool isServerRunning;
  bool newImageUploaded = false;
  bool refreshRequested = false;

  String htmlTemplate;
  OnSaveCallback onSaveCallback;
  OnPinCallback onPinCallback;

  void setupWebServer();
  void setupDNSServer();
  String getConfigurationPage();
  bool loadHtmlTemplate();
  void handleRoot(AsyncWebServerRequest *request);
  void handleSave(AsyncWebServerRequest *request);
  void handleNotFound(AsyncWebServerRequest *request);
  void handleUpload(AsyncWebServerRequest *request, const String &filename,
                    size_t index, uint8_t *data, size_t len, bool final);
  void handleFolderImages(AsyncWebServerRequest *request);
  void handlePinImage(AsyncWebServerRequest *request);
};

#endif
