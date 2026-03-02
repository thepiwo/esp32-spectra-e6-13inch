#include "ConfigurationServer.h"

#include "FolderImageSource.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiAP.h>

const char *ConfigurationServer::WIFI_AP_NAME = "Framey-Config";
const char *ConfigurationServer::WIFI_AP_PASSWORD = "configure123";

ConfigurationServer::ConfigurationServer(const Configuration &currentConfig)
    : deviceName("E-Ink-Display"), wifiAccessPointName(WIFI_AP_NAME),
      wifiAccessPointPassword(WIFI_AP_PASSWORD),
      currentConfiguration(currentConfig), server(nullptr), dnsServer(nullptr),
      isServerRunning(false) {}

void ConfigurationServer::run(OnSaveCallback onSaveCallback, bool startAP) {
  this->onSaveCallback = onSaveCallback;

  Serial.println("Starting Configuration Server...");
  Serial.print("Device Name: ");
  Serial.println(deviceName);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed - filesystem must be uploaded first");
    return;
  }
  Serial.println("LittleFS initialized successfully");

  if (!loadHtmlTemplate()) {
    Serial.println("Failed to load HTML template");
    LittleFS.end();
    return;
  }
  Serial.println("HTML template loaded successfully");
  LittleFS.end();

  if (startAP) {
    WiFi.disconnect(true);
    delay(1000);

    Serial.print("Setting up WiFi Access Point: ");
    Serial.println(wifiAccessPointName);

    WiFi.mode(WIFI_AP);
    bool apStarted = WiFi.softAP(wifiAccessPointName.c_str(),
                                 wifiAccessPointPassword.c_str());

    if (apStarted) {
      Serial.println("Access Point started successfully!");
      Serial.print("Network Name (SSID): ");
      Serial.println(wifiAccessPointName);
      Serial.print("Password: ");
      Serial.println(wifiAccessPointPassword);
      Serial.print("Access Point IP: ");
      Serial.println(WiFi.softAPIP());
      Serial.println("Setting up captive portal...");

      setupDNSServer();
      setupWebServer();

      isServerRunning = true;
      Serial.println("Captive portal is running!");
      Serial.println("Devices connecting to this network will be automatically "
                     "redirected to the configuration page");
    } else {
      Serial.println("Failed to start Access Point!");
    }
  } else {
    // Start only the web server on the existing local WiFi connection
    setupWebServer();
    isServerRunning = true;
    Serial.println("Web Server running on local WiFi network!");
    Serial.print("Access it at: http://");
    Serial.println(WiFi.localIP());
  }
}

void ConfigurationServer::stop() {
  if (isServerRunning) {
    if (server) {
      delete server;
      server = nullptr;
    }
    if (dnsServer) {
      dnsServer->stop();
      delete dnsServer;
      dnsServer = nullptr;
    }
    WiFi.softAPdisconnect(true);
    isServerRunning = false;
    Serial.println("Configuration server stopped");
  }
}

void ConfigurationServer::handleRequests() {
  if (isServerRunning && dnsServer) {
    dnsServer->processNextRequest();
  }
}

void ConfigurationServer::setupDNSServer() {
  dnsServer = new DNSServer();
  const byte DNS_PORT = 53;
  dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("DNS Server started - all domains redirect to captive portal");
}

void ConfigurationServer::setupWebServer() {
  server = new AsyncWebServer(80);

  server->on("/generate_204", HTTP_GET, [this](AsyncWebServerRequest *request) {
    handleRoot(request);
  }); // Android
  server->on("/fwlink", HTTP_GET, [this](AsyncWebServerRequest *request) {
    handleRoot(request);
  }); // Microsoft
  server->on(
      "/hotspot-detect.html", HTTP_GET,
      [this](AsyncWebServerRequest *request) { handleRoot(request); }); // iOS
  server->on("/connectivity-check.html", HTTP_GET,
             [this](AsyncWebServerRequest *request) {
               handleRoot(request);
             }); // Firefox

  server->on("/", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });
  server->on("/config", HTTP_GET,
             [this](AsyncWebServerRequest *request) { handleRoot(request); });
  server->on("/save", HTTP_POST,
             [this](AsyncWebServerRequest *request) { handleSave(request); });

  server->on(
      "/upload", HTTP_POST,
      [this](AsyncWebServerRequest *request) {
        newImageUploaded = true;
        request->send(200, "text/plain",
                      "Upload successful! Refreshing display...");
      },
      [this](AsyncWebServerRequest *request, const String &filename,
             size_t index, uint8_t *data, size_t len, bool final) {
        this->handleUpload(request, filename, index, data, len, final);
      });

  server->on(
      "/api/folder-images", HTTP_GET,
      [this](AsyncWebServerRequest *request) { handleFolderImages(request); });
  server->on(
      "/api/pin-image", HTTP_POST,
      [this](AsyncWebServerRequest *request) { handlePinImage(request); });
  server->on("/api/refresh-display", HTTP_POST,
             [this](AsyncWebServerRequest *request) {
               refreshRequested = true;
               Serial.println("Display refresh requested via web UI");
               request->send(200, "application/json", "{\"ok\":true}");
             });

  server->on("/clear", HTTP_POST, [this](AsyncWebServerRequest *request) {
    if (LittleFS.begin(true)) {
      const char *extensions[] = {".bmp", ".jpg", ".jpeg", ".png"};
      bool deleted = false;
      for (const char *ext : extensions) {
        String path = "/local_image" + String(ext);
        if (LittleFS.exists(path)) {
          if (LittleFS.remove(path)) {
            Serial.println("Deleted: " + path);
            deleted = true;
          }
        }
      }
      if (deleted) {
        request->send(200, "text/plain", "Local image cleared. Rebooting...");
      } else {
        request->send(200, "text/plain",
                      "No local image to clear. Rebooting...");
      }
      LittleFS.end();
      delay(500);
      ESP.restart();
    } else {
      request->send(500, "text/plain", "LittleFS error");
    }
  });

  server->onNotFound(
      [this](AsyncWebServerRequest *request) { handleNotFound(request); });

  server->begin();
  Serial.println("Web server started on port 80");
}

void ConfigurationServer::handleRoot(AsyncWebServerRequest *request) {
  String html = getConfigurationPage();
  request->send(200, "text/html", html);
}

void ConfigurationServer::handleSave(AsyncWebServerRequest *request) {
  if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
    Configuration config;
    config.ssid = request->getParam("ssid", true)->value();
    config.password = request->getParam("password", true)->value();

    if (request->hasParam("imageUrl", true))
      config.imageUrl = request->getParam("imageUrl", true)->value();
    if (request->hasParam("folderUrl", true))
      config.folderUrl = request->getParam("folderUrl", true)->value();
    if (request->hasParam("ditherMode", true))
      config.ditherMode =
          request->getParam("ditherMode", true)->value().toInt();
    if (request->hasParam("scalingMode", true))
      config.scalingMode =
          request->getParam("scalingMode", true)->value().toInt();
    if (request->hasParam("sleepMinutes", true))
      config.sleepMinutes =
          request->getParam("sleepMinutes", true)->value().toInt();
    if (request->hasParam("imageChangeMinutes", true))
      config.imageChangeMinutes =
          request->getParam("imageChangeMinutes", true)->value().toInt();
    if (request->hasParam("pinnedImageUrl", true))
      config.pinnedImageUrl =
          request->getParam("pinnedImageUrl", true)->value();

    /*
    // Auto-clear pin if folder URL changed — keeping it for now per user
    preference to stick to image if (config.folderUrl !=
    currentConfiguration.folderUrl) { config.pinnedImageUrl = "";
      Serial.println("Folder URL changed — pinned image cleared");
    }
    */

    Serial.println("Configuration received");
    request->send(200, "text/plain", "OK");

    onSaveCallback(config);
  } else {
    request->send(400, "text/plain", "Missing parameters");
  }

  // Server stops removed — let the server stay alive for more pinning/cycle
  // checks stop();
}

void ConfigurationServer::handleNotFound(AsyncWebServerRequest *request) {
  request->redirect("/");
}

bool ConfigurationServer::loadHtmlTemplate() {
  File file = LittleFS.open("/config.html", "r");
  if (!file) {
    Serial.println("Failed to open config.html file");
    return false;
  }

  htmlTemplate = file.readString();
  file.close();

  if (htmlTemplate.length() == 0) {
    Serial.println("config.html file is empty");
    return false;
  }

  return true;
}

static void setSelected(String &html, const String &placeholder,
                        bool selected) {
  html.replace(placeholder, selected ? "selected" : "");
}

String ConfigurationServer::getConfigurationPage() {
  String html = htmlTemplate;
  html.replace("{{CURRENT_SSID}}", currentConfiguration.ssid);
  html.replace("{{CURRENT_PASSWORD}}", currentConfiguration.password);
  html.replace("{{CURRENT_IMAGE_URL}}", currentConfiguration.imageUrl);
  html.replace("{{CURRENT_FOLDER_URL}}", currentConfiguration.folderUrl);
  html.replace("{{CURRENT_PINNED_IMAGE_URL}}",
               currentConfiguration.pinnedImageUrl);

  // Dithering dropdown
  uint8_t dm = currentConfiguration.ditherMode;
  setSelected(html, "{{DITHER_SEL_0}}", dm == 0);
  setSelected(html, "{{DITHER_SEL_1}}", dm == 1);
  setSelected(html, "{{DITHER_SEL_2}}", dm == 2);
  setSelected(html, "{{DITHER_SEL_3}}", dm == 3);

  // Scaling Mode dropdown
  uint8_t sm = currentConfiguration.scalingMode;
  setSelected(html, "{{SCALE_SEL_0}}", sm == 0);
  setSelected(html, "{{SCALE_SEL_1}}", sm == 1);

  // Image change interval dropdown
  uint16_t ic = currentConfiguration.imageChangeMinutes;
  setSelected(html, "{{IMG_CHG_SEL_0}}", ic == 0);
  setSelected(html, "{{IMG_CHG_SEL_1}}", ic == 1);
  setSelected(html, "{{IMG_CHG_SEL_15}}", ic == 15);
  setSelected(html, "{{IMG_CHG_SEL_30}}", ic == 30);
  setSelected(html, "{{IMG_CHG_SEL_60}}", ic == 60);
  setSelected(html, "{{IMG_CHG_SEL_120}}", ic == 120);
  setSelected(html, "{{IMG_CHG_SEL_360}}", ic == 360);
  setSelected(html, "{{IMG_CHG_SEL_720}}", ic == 720);
  setSelected(html, "{{IMG_CHG_SEL_1440}}", ic == 1440);

  // Sleep interval dropdown
  uint16_t sl = currentConfiguration.sleepMinutes;
  setSelected(html, "{{SLEEP_SEL_0}}", sl == 0);
  setSelected(html, "{{SLEEP_SEL_15}}", sl == 15);
  setSelected(html, "{{SLEEP_SEL_30}}", sl == 30);
  setSelected(html, "{{SLEEP_SEL_60}}", sl == 60);
  setSelected(html, "{{SLEEP_SEL_120}}", sl == 120);
  setSelected(html, "{{SLEEP_SEL_240}}", sl == 240);
  setSelected(html, "{{SLEEP_SEL_360}}", sl == 360);
  setSelected(html, "{{SLEEP_SEL_720}}", sl == 720);
  setSelected(html, "{{SLEEP_SEL_1440}}", sl == 1440);

  return html;
}

String ConfigurationServer::getWifiAccessPointName() const {
  return wifiAccessPointName;
}

String ConfigurationServer::getWifiAccessPointPassword() const {
  return wifiAccessPointPassword;
}

bool ConfigurationServer::isRunning() const { return isServerRunning; }

void ConfigurationServer::handleUpload(AsyncWebServerRequest *request,
                                       const String &filename, size_t index,
                                       uint8_t *data, size_t len, bool final) {
  static File uploadFile;
  if (!index) {
    Serial.printf("UploadStart: %s\n", filename.c_str());

    // Determine extension
    String ext = "";
    int lastDot = filename.lastIndexOf('.');
    if (lastDot != -1) {
      ext = filename.substring(lastDot);
      ext.toLowerCase();
    }

    // Delete any existing local images first to avoid clutter
    if (LittleFS.begin(true)) {
      const char *extensions[] = {".bmp", ".jpg", ".jpeg", ".png"};
      for (const char *e : extensions) {
        String path = "/local_image" + String(e);
        if (LittleFS.exists(path))
          LittleFS.remove(path);
      }
    }

    String uploadPath = "/local_image" + ext;
    uploadFile = LittleFS.open(uploadPath, FILE_WRITE);
    if (!uploadFile) {
      Serial.println("Failed to open " + uploadPath +
                     " for writing in LittleFS");
    }
  }

  if (uploadFile) {
    uploadFile.write(data, len);
  }

  if (final) {
    Serial.printf("UploadEnd: %s, %u B\n", filename.c_str(), index + len);
    if (uploadFile) {
      uploadFile.close();
    }
  }
}

void ConfigurationServer::handleFolderImages(AsyncWebServerRequest *request) {
  String folderUrl = currentConfiguration.folderUrl;
  if (folderUrl.length() == 0) {
    request->send(400, "application/json",
                  "{\"error\":\"No folder URL configured\"}");
    return;
  }

  FolderImageSource folderSource;
  String html = folderSource.fetchDirectoryListing(folderUrl);
  if (html.length() == 0) {
    request->send(502, "application/json",
                  "{\"error\":\"Failed to fetch folder listing\"}");
    return;
  }

  auto imageUrls = folderSource.parseImageLinks(html, folderUrl);

  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  response->print("[");
  for (size_t i = 0; i < imageUrls.size(); i++) {
    if (i > 0)
      response->print(",");
    String url = imageUrls[i];
    int lastSlash = url.lastIndexOf('/');
    String name = (lastSlash >= 0) ? url.substring(lastSlash + 1) : url;
    // Escape any quotes in name/url for valid JSON
    name.replace("\"", "\\\"");
    String escapedUrl = url;
    escapedUrl.replace("\"", "\\\"");
    response->printf("{\"name\":\"%s\",\"url\":\"%s\"}", name.c_str(),
                     escapedUrl.c_str());
  }
  response->print("]");
  request->send(response);
}

void ConfigurationServer::handlePinImage(AsyncWebServerRequest *request) {
  String pinnedUrl = "";
  if (request->hasParam("url", true)) {
    pinnedUrl = request->getParam("url", true)->value();
  }

  currentConfiguration.pinnedImageUrl = pinnedUrl;

  if (onPinCallback) {
    onPinCallback(pinnedUrl);
  }

  if (pinnedUrl.length() > 0) {
    Serial.printf("Image pinned: %s\n", pinnedUrl.c_str());
  } else {
    Serial.println("Image unpinned — cycling resumed");
  }

  request->send(200, "application/json", "{\"ok\":true}");
}
