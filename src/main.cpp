#include <Arduino.h>

#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "ConfigurationScreen.h"
#include "ConfigurationServer.h"
#include "DisplayAdapter.h"
#include "ImageScreen.h"
#include "SDCardManager.h"
#include "WiFiConnection.h"
#include "battery.h"
#include <SPI.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

// Global display instance
DisplayType display;
ApplicationConfigStorage configStorage;
std::unique_ptr<ApplicationConfig> appConfig;

// Survives deep sleep — tracks wake cycles for image change interval
RTC_DATA_ATTR static uint16_t wakesSinceImageChange = 0;

void initializeDefaultConfig() {
  std::unique_ptr<ApplicationConfig> storedConfig = configStorage.load();
  if (storedConfig) {
    appConfig = std::move(storedConfig);
    // Force FILL mode — user can change via web UI if needed
    appConfig->scalingMode = SCALE_FILL;
    printf("Configuration loaded from persistent storage: \r\n");
    printf("  - WiFi SSID: %s\n", appConfig->wifiSSID);
    printf("  - Scaling mode: %s\n",
           appConfig->scalingMode == SCALE_FILL ? "FILL" : "FIT");
  } else {
    appConfig.reset(new ApplicationConfig());
    printf("Using default configuration (no stored config found) \r\n");
  }
}

int displayCurrentScreen(bool isConnected) {
  if (!appConfig->hasValidWiFiCredentials()) {
    printf("No WiFi credentials, showing configuration screen... \r\n");
    ConfigurationScreen configScreen(display);
    configScreen.render();
    return configScreen.nextRefreshInSeconds();
  }

  printf("Showing image screen... \r\n");
  ImageScreen imageScreen(display, *appConfig, configStorage);
  imageScreen.render();
  return imageScreen.nextRefreshInSeconds();
}

void setup() {
  Serial.begin(115200);
  // Wait for Serial monitor to connect
  unsigned long start = millis();
  while (!Serial && (millis() - start < 3000))
    delay(10);

  printf("\r\n\r\n--- ESP32-133C02 STARTING ---\r\n");
  esp_reset_reason_t reason = esp_reset_reason();
  printf("Reset reason: %d \r\n", (int)reason);
  if (reason == ESP_RST_BROWNOUT) {
    printf("WARNING: Brownout reset detected! Check power supply.\r\n");
  }

  printf("ESP32-133C02 E-Ink Spectra 6 (13.3\") starting... \r\n");
  fflush(stdout);

  initializeDefaultConfig();

  // --- Timer Wake Detection ---
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  bool timerWake = (wakeupCause == ESP_SLEEP_WAKEUP_TIMER);

  if (timerWake) {
    printf("Timer wake-up detected.\r\n");

    // Advance folder image index if enough time has passed
    // Skip cycling when an image is pinned — pinned images don't rotate
    if (appConfig->hasFolderUrl() && !appConfig->hasPinnedImage() &&
        appConfig->sleepMinutes > 0) {
      wakesSinceImageChange++;
      bool shouldChange =
          (appConfig->imageChangeMinutes == 0) ||
          ((uint32_t)wakesSinceImageChange * appConfig->sleepMinutes >=
           appConfig->imageChangeMinutes);

      if (shouldChange) {
        uint16_t idx = configStorage.loadImageIndex();
        configStorage.saveImageIndex(idx + 1);
        wakesSinceImageChange = 0;
        printf("Folder: advanced image index to %d\r\n", idx + 1);
      } else {
        printf("Image change: %d/%d minutes elapsed\r\n",
               wakesSinceImageChange * appConfig->sleepMinutes,
               appConfig->imageChangeMinutes);
      }
    } else if (appConfig->hasPinnedImage()) {
      printf("Pinned image active — skipping cycling\r\n");
    }
  }

  // --- SD Card Phase ---
  // The SD card shares SPI pins with the display, so we access it FIRST
  // (before the display driver initialises the SPI bus). If an image is
  // found it is copied to LittleFS; the SPI bus is then fully released
  // so the display can claim it later.
  copyImageFromSDToLittleFS();

  // Connect to WiFi
  if (appConfig->hasValidWiFiCredentials()) {
    WiFiConnection wifi(appConfig->wifiSSID, appConfig->wifiPassword);
    printf("Connecting to WiFi: SSID='%s' \r\n", appConfig->wifiSSID);
    wifi.connect();

    int retry = 0;
    while (!wifi.isConnected() && retry < 20) {
      printf(".");
      delay(1000);
      retry++;
    }
    printf("\r\n");

    if (wifi.isConnected()) {
      printf("WiFi Connected! IP: %s \r\n", WiFi.localIP().toString().c_str());
    } else {
      printf("WiFi Connection Failed. \r\n");
    }

    // --- Display Image Phase (FIRST) ---
    printf("Entering displayCurrentScreen()... \r\n");
    displayCurrentScreen(wifi.isConnected());
    printf("Image displayed successfully.\r\n");

    // --- Web Server Phase (skip on timer wake for fast sleep cycling) ---
    if (!timerWake) {
      Configuration serverConfig(
          appConfig->wifiSSID, appConfig->wifiPassword, appConfig->imageUrl,
          appConfig->folderUrl, appConfig->pinnedImageUrl,
          appConfig->ditherMode, appConfig->scalingMode,
          appConfig->sleepMinutes, appConfig->imageChangeMinutes);
      ConfigurationServer server(serverConfig);

      bool useAP = !wifi.isConnected();
      if (useAP) {
        printf("WiFi failed. Starting Access Point mode...\r\n");
      }

      server.run(
          [](const Configuration &config) {
            printf("Configuration received: SSID=%s, URL=%s, Folder=%s\r\n",
                   config.ssid.c_str(), config.imageUrl.c_str(),
                   config.folderUrl.c_str());

            strncpy(appConfig->wifiSSID, config.ssid.c_str(),
                    sizeof(appConfig->wifiSSID) - 1);
            strncpy(appConfig->wifiPassword, config.password.c_str(),
                    sizeof(appConfig->wifiPassword) - 1);
            strncpy(appConfig->imageUrl, config.imageUrl.c_str(),
                    sizeof(appConfig->imageUrl) - 1);
            strncpy(appConfig->folderUrl, config.folderUrl.c_str(),
                    sizeof(appConfig->folderUrl) - 1);
            strncpy(appConfig->pinnedImageUrl, config.pinnedImageUrl.c_str(),
                    sizeof(appConfig->pinnedImageUrl) - 1);
            appConfig->ditherMode = config.ditherMode;
            appConfig->scalingMode = config.scalingMode;
            appConfig->sleepMinutes = config.sleepMinutes;
            appConfig->imageChangeMinutes = config.imageChangeMinutes;

            if (configStorage.save(*appConfig)) {
              printf("Configuration saved successfully to NVS.\r\n");
            } else {
              printf("ERROR: Failed to save configuration to NVS.\r\n");
            }
          },
          useAP);

      server.setOnPinCallback([](const String &pinnedUrl) {
        strncpy(appConfig->pinnedImageUrl, pinnedUrl.c_str(),
                sizeof(appConfig->pinnedImageUrl) - 1);
        configStorage.save(*appConfig);
        printf("Pinned image saved to NVS: %s\r\n", pinnedUrl.c_str());
      });

      // Run web server for 10 minutes (600,000 ms)
      const unsigned long SERVER_TIMEOUT_MS = 10UL * 60UL * 1000UL;
      unsigned long serverStart = millis();
      printf("Web server running for 10 minutes. Upload images now!\r\n");
      printf("Access it at: http://%s\r\n",
             useAP ? "192.168.4.1" : WiFi.localIP().toString().c_str());

      while (millis() - serverStart < SERVER_TIMEOUT_MS) {
        server.handleRequests();

        if (server.hasNewImage()) {
          server.clearNewImage();
          printf("New image uploaded! Refreshing display...\r\n");
          displayCurrentScreen(wifi.isConnected());
          printf("Display refreshed with new image.\r\n");
        }

        if (server.isRefreshRequested()) {
          server.clearRefreshRequest();
          printf("Display refresh requested! Refreshing...\r\n");
          displayCurrentScreen(wifi.isConnected());
          printf("Display refreshed.\r\n");
        }

        delay(10);
      }

      printf("Web server timeout reached.\r\n");
    } else {
      printf("Timer wake — skipping web server.\r\n");
    }

  } else {
    // No WiFi credentials — show image first, then start AP for setup
    printf("No WiFi credentials. Displaying image first...\r\n");
    displayCurrentScreen(false);

    if (!timerWake) {
      printf("Starting Access Point mode...\r\n");
      Configuration serverConfig(
          "", "", appConfig->imageUrl, appConfig->folderUrl,
          appConfig->pinnedImageUrl, appConfig->ditherMode,
          appConfig->scalingMode, appConfig->sleepMinutes,
          appConfig->imageChangeMinutes);
      ConfigurationServer server(serverConfig);
      server.run(
          [](const Configuration &config) {
            printf("Configuration received (AP mode): SSID=%s\r\n",
                   config.ssid.c_str());
            strncpy(appConfig->wifiSSID, config.ssid.c_str(),
                    sizeof(appConfig->wifiSSID) - 1);
            strncpy(appConfig->wifiPassword, config.password.c_str(),
                    sizeof(appConfig->wifiPassword) - 1);
            strncpy(appConfig->imageUrl, config.imageUrl.c_str(),
                    sizeof(appConfig->imageUrl) - 1);
            strncpy(appConfig->folderUrl, config.folderUrl.c_str(),
                    sizeof(appConfig->folderUrl) - 1);
            strncpy(appConfig->pinnedImageUrl, config.pinnedImageUrl.c_str(),
                    sizeof(appConfig->pinnedImageUrl) - 1);
            appConfig->ditherMode = config.ditherMode;
            appConfig->scalingMode = config.scalingMode;
            appConfig->sleepMinutes = config.sleepMinutes;
            appConfig->imageChangeMinutes = config.imageChangeMinutes;

            if (configStorage.save(*appConfig)) {
              printf("Configuration saved successfully from AP mode.\r\n");
            }
          },
          true);

      server.setOnPinCallback([](const String &pinnedUrl) {
        strncpy(appConfig->pinnedImageUrl, pinnedUrl.c_str(),
                sizeof(appConfig->pinnedImageUrl) - 1);
        configStorage.save(*appConfig);
        printf("Pinned image saved to NVS (AP mode): %s\r\n",
               pinnedUrl.c_str());
      });

      // Run AP server for 10 minutes
      const unsigned long SERVER_TIMEOUT_MS = 10UL * 60UL * 1000UL;
      unsigned long serverStart = millis();
      printf("AP server running for 10 minutes.\r\n");

      while (millis() - serverStart < SERVER_TIMEOUT_MS) {
        server.handleRequests();

        if (server.hasNewImage()) {
          server.clearNewImage();
          printf("New image uploaded! Refreshing display...\r\n");
          displayCurrentScreen(false);
          printf("Display refreshed with new image.\r\n");
        }

        if (server.isRefreshRequested()) {
          server.clearRefreshRequest();
          printf("Display refresh requested (AP)! Refreshing...\r\n");
          displayCurrentScreen(false);
          printf("Display refreshed.\r\n");
        }

        delay(10);
      }

      printf("AP server timeout.\r\n");
    }
  }

  // --- Deep Sleep ---
  if (appConfig->sleepMinutes > 0) {
    uint64_t sleepUs = (uint64_t)appConfig->sleepMinutes * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleepUs);
    printf("Timed deep sleep for %d minutes. Device will wake "
           "automatically.\r\n",
           appConfig->sleepMinutes);
  } else {
    printf("Permanent deep sleep. Reset to wake.\r\n");
  }
  esp_deep_sleep_start();
}

void loop() {}
