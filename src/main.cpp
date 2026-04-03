#include <Arduino.h>
#include <time.h>

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
    printf("Configuration loaded from persistent storage: \r\n");
    printf("  - WiFi SSID: %s\n", appConfig->wifiSSID);
    printf("  - Scaling mode: %s (%d)\n",
           appConfig->scalingMode == SCALE_FILL ? "FILL" : "FIT",
           appConfig->scalingMode);
    printf("  - Pinned Image: %s\n", appConfig->pinnedImageUrl);
    printf("  - Folder URL: %s\n", appConfig->folderUrl);
  } else {
    appConfig.reset(new ApplicationConfig());
    printf("Using default configuration (no stored config found) \r\n");
    printf("  - Default Scaling mode: %d\n", appConfig->scalingMode);
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

    // --- NTP Sync + Quiet Hours Gate (timer wakes only) ---
    bool quietHoursEnabled =
        (appConfig->quietHoursStart != appConfig->quietHoursEnd);
    bool gotTime = false;
    struct tm timeinfo;

    if (timerWake && quietHoursEnabled && wifi.isConnected()) {
      // Only sync NTP when quiet hours are configured and we need to check them
      configTime((long)appConfig->utcOffsetHours * 3600L, 0, "pool.ntp.org");
      // dstOffsetSec = 0: DST not supported, use UTC offset only
      gotTime = getLocalTime(&timeinfo, 5000);
      if (gotTime) {
        printf("NTP time: %02d:%02d:%02d\r\n", timeinfo.tm_hour,
               timeinfo.tm_min, timeinfo.tm_sec);
      } else {
        printf("NTP sync failed\r\n");
      }
    }

    if (timerWake && quietHoursEnabled) {
      if (!gotTime) {
        // WiFi failed or NTP timed out — cannot confirm time, sleep and retry.
        // Note: display is skipped on WiFi failures during quiet hours (intentional).
        uint16_t fallback = appConfig->sleepMinutes;
        if (fallback == 0) fallback = appConfig->imageChangeMinutes;
        if (fallback == 0) fallback = 30; // hard floor: never permanent-sleep
        printf("Quiet hours: no time available, sleeping %d min\r\n", fallback);
        esp_sleep_enable_timer_wakeup((uint64_t)fallback * 60ULL * 1000000ULL);
        esp_deep_sleep_start();
      }

      uint8_t h  = timeinfo.tm_hour;
      uint8_t qs = appConfig->quietHoursStart;
      uint8_t qe = appConfig->quietHoursEnd;
      bool inQuiet = (qs < qe) ? (h >= qs && h < qe) : (h >= qs || h < qe);

      if (inQuiet) {
        uint32_t secsLeft;
        if (qe > h) {
          secsLeft = (uint32_t)(qe - h) * 3600u
                     - (uint32_t)timeinfo.tm_min * 60u
                     - (uint32_t)timeinfo.tm_sec;
        } else { // midnight-spanning: end hour is tomorrow
          secsLeft = (uint32_t)(24u - h + qe) * 3600u
                     - (uint32_t)timeinfo.tm_min * 60u
                     - (uint32_t)timeinfo.tm_sec;
        }
        printf("Quiet hours active (%02d:xx -> %02d:00). Sleeping %u s.\r\n",
               h, qe, secsLeft);
        esp_sleep_enable_timer_wakeup((uint64_t)secsLeft * 1000000ULL);
        esp_deep_sleep_start();
      }
    }

    // --- Image Index Advance (after quiet hours gate — skipped wakes don't count) ---
    if (timerWake) {
      printf("Timer wake-up detected.\r\n");
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
          appConfig->sleepMinutes, appConfig->imageChangeMinutes,
          appConfig->quietHoursStart, appConfig->quietHoursEnd,
          appConfig->utcOffsetHours);
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
            appConfig->wifiSSID[sizeof(appConfig->wifiSSID) - 1] = '\0';

            strncpy(appConfig->wifiPassword, config.password.c_str(),
                    sizeof(appConfig->wifiPassword) - 1);
            appConfig->wifiPassword[sizeof(appConfig->wifiPassword) - 1] = '\0';

            strncpy(appConfig->imageUrl, config.imageUrl.c_str(),
                    sizeof(appConfig->imageUrl) - 1);
            appConfig->imageUrl[sizeof(appConfig->imageUrl) - 1] = '\0';

            strncpy(appConfig->folderUrl, config.folderUrl.c_str(),
                    sizeof(appConfig->folderUrl) - 1);
            appConfig->folderUrl[sizeof(appConfig->folderUrl) - 1] = '\0';

            strncpy(appConfig->pinnedImageUrl, config.pinnedImageUrl.c_str(),
                    sizeof(appConfig->pinnedImageUrl) - 1);
            appConfig->pinnedImageUrl[sizeof(appConfig->pinnedImageUrl) - 1] =
                '\0';
            appConfig->ditherMode = config.ditherMode;
            appConfig->scalingMode = config.scalingMode;
            appConfig->sleepMinutes = config.sleepMinutes;
            appConfig->imageChangeMinutes = config.imageChangeMinutes;
            appConfig->quietHoursStart = config.quietHoursStart;
            appConfig->quietHoursEnd   = config.quietHoursEnd;
            appConfig->utcOffsetHours  = config.utcOffsetHours;

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
        appConfig->pinnedImageUrl[sizeof(appConfig->pinnedImageUrl) - 1] = '\0';
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

    // Image index advance (no WiFi: quiet hours don't apply, always advance)
    if (timerWake) {
      printf("Timer wake-up detected.\r\n");
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

    displayCurrentScreen(false);

    if (!timerWake) {
      printf("Starting Access Point mode...\r\n");
      Configuration serverConfig(
          "", "", appConfig->imageUrl, appConfig->folderUrl,
          appConfig->pinnedImageUrl, appConfig->ditherMode,
          appConfig->scalingMode, appConfig->sleepMinutes,
          appConfig->imageChangeMinutes,
          appConfig->quietHoursStart, appConfig->quietHoursEnd,
          appConfig->utcOffsetHours);
      ConfigurationServer server(serverConfig);
      server.run(
          [](const Configuration &config) {
            printf("Configuration received (AP mode): SSID=%s\r\n",
                   config.ssid.c_str());
            strncpy(appConfig->wifiSSID, config.ssid.c_str(),
                    sizeof(appConfig->wifiSSID) - 1);
            appConfig->wifiSSID[sizeof(appConfig->wifiSSID) - 1] = '\0';

            strncpy(appConfig->wifiPassword, config.password.c_str(),
                    sizeof(appConfig->wifiPassword) - 1);
            appConfig->wifiPassword[sizeof(appConfig->wifiPassword) - 1] = '\0';

            strncpy(appConfig->imageUrl, config.imageUrl.c_str(),
                    sizeof(appConfig->imageUrl) - 1);
            appConfig->imageUrl[sizeof(appConfig->imageUrl) - 1] = '\0';

            strncpy(appConfig->folderUrl, config.folderUrl.c_str(),
                    sizeof(appConfig->folderUrl) - 1);
            appConfig->folderUrl[sizeof(appConfig->folderUrl) - 1] = '\0';

            strncpy(appConfig->pinnedImageUrl, config.pinnedImageUrl.c_str(),
                    sizeof(appConfig->pinnedImageUrl) - 1);
            appConfig->pinnedImageUrl[sizeof(appConfig->pinnedImageUrl) - 1] =
                '\0';
            appConfig->ditherMode = config.ditherMode;
            appConfig->scalingMode = config.scalingMode;
            appConfig->sleepMinutes = config.sleepMinutes;
            appConfig->imageChangeMinutes = config.imageChangeMinutes;
            appConfig->quietHoursStart = config.quietHoursStart;
            appConfig->quietHoursEnd   = config.quietHoursEnd;
            appConfig->utcOffsetHours  = config.utcOffsetHours;

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
  uint16_t effectiveSleepMinutes = appConfig->sleepMinutes;

  // For single image URL (no folder), use imageChangeMinutes as the wake
  // interval so the device periodically re-downloads the image.
  if (effectiveSleepMinutes == 0 && strlen(appConfig->imageUrl) > 0 &&
      !appConfig->hasFolderUrl() && appConfig->imageChangeMinutes > 0) {
    effectiveSleepMinutes = appConfig->imageChangeMinutes;
    printf("Using imageChangeMinutes (%d min) as sleep timer for single image "
           "URL.\r\n",
           effectiveSleepMinutes);
  }

  if (effectiveSleepMinutes > 0) {
    uint64_t sleepUs = (uint64_t)effectiveSleepMinutes * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleepUs);
    printf("Timed deep sleep for %d minutes. Device will wake "
           "automatically.\r\n",
           effectiveSleepMinutes);
  } else {
    printf("Permanent deep sleep. Reset to wake.\r\n");
  }
  esp_deep_sleep_start();
}

void loop() {}
