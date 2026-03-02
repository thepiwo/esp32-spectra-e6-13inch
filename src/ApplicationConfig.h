#pragma once

#include <Arduino.h>

#if __has_include("config_dev.h")
#include "config_dev.h"
#else
#include "config_default.h"
#endif

enum ScreenType { CONFIG_SCREEN = 0, IMAGE_SCREEN = 1, SCREEN_COUNT = 2 };

// Dithering algorithm options
enum DitherMode : uint8_t {
  DITHER_FLOYD_STEINBERG = 0,
  DITHER_ATKINSON = 1,
  DITHER_ORDERED = 2,
  DITHER_NONE = 3,
};

// Image Scaling Options
enum ScalingMode : uint8_t {
  SCALE_FIT = 0,  // Letterbox: keep entire image visible
  SCALE_FILL = 1, // Crop: completely fill screen, cutting off edges
};

struct ApplicationConfig {
  char wifiSSID[64];
  char wifiPassword[64];
  char imageUrl[300];
  char folderUrl[300];      // HTTP folder URL for image cycling
  char pinnedImageUrl[300]; // Full URL of a pinned folder image (empty =
                            // cycling)
  uint8_t ditherMode;       // DitherMode enum value
  uint8_t scalingMode;      // ScalingMode enum value (0=fit, 1=fill)
  uint16_t
      sleepMinutes; // 0 = no timed wake (permanent sleep after server timeout)
  uint16_t imageChangeMinutes; // How often to advance to the next image (0 =
                               // every wake)

  static const int DISPLAY_ROTATION = 2;

  ApplicationConfig() {
    memset(wifiSSID, 0, sizeof(wifiSSID));
    memset(wifiPassword, 0, sizeof(wifiPassword));
    memset(imageUrl, 0, sizeof(imageUrl));
    memset(folderUrl, 0, sizeof(folderUrl));
    memset(pinnedImageUrl, 0, sizeof(pinnedImageUrl));

    strncpy(wifiSSID, DEFAULT_WIFI_SSID, sizeof(wifiSSID) - 1);
    strncpy(wifiPassword, DEFAULT_WIFI_PASSWORD, sizeof(wifiPassword) - 1);
    strncpy(imageUrl, DEFAULT_IMAGE_URL, sizeof(imageUrl) - 1);

    ditherMode = DITHER_FLOYD_STEINBERG;
    scalingMode = SCALE_FIT; // Default to fit/letterbox (preserve entire image)
    sleepMinutes = 0;
    imageChangeMinutes = 1;
  }

  bool hasValidWiFiCredentials() const {
    return strlen(wifiSSID) > 0 && strlen(wifiPassword) > 0;
  }
  bool hasFolderUrl() const { return strlen(folderUrl) > 0; }
  bool hasPinnedImage() const { return strlen(pinnedImageUrl) > 0; }
};
