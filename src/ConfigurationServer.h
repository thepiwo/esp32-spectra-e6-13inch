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
  uint16_t sleepMinutes = 0;
  uint16_t imageChangeMinutes = 30;

  Configuration() = default;

  Configuration(const String &ssid, const String &password,
                const String &imageUrl, const String &folderUrl = "",
                const String &pinnedImageUrl = "",
                uint8_t ditherMode = 0, uint16_t sleepMinutes = 0,
                uint16_t imageChangeMinutes = 30)
      : ssid(ssid), password(password), imageUrl(imageUrl),
        folderUrl(folderUrl), pinnedImageUrl(pinnedImageUrl),
        ditherMode(ditherMode), sleepMinutes(sleepMinutes),
        imageChangeMinutes(imageChangeMinutes) {}
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

private:
  String deviceName;
  String wifiAccessPointName;
  String wifiAccessPointPassword;

  Configuration currentConfiguration;

  AsyncWebServer *server;
  DNSServer *dnsServer;
  bool isServerRunning;
  bool newImageUploaded = false;

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
