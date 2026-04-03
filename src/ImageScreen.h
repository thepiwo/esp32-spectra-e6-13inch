#ifndef IMAGE_SCREEN_H
#define IMAGE_SCREEN_H

#include <Arduino.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <esp_attr.h>
#include <memory>
#include <stdint.h>


#include "ApplicationConfig.h"
#include "ApplicationConfigStorage.h"
#include "DisplayAdapter.h"
#include "HttpDownloader.h"
#include "Screen.h"
#include <FS.h>
#include <LittleFS.h>

RTC_DATA_ATTR static char storedImageETag[128] = "";

struct ColorImageBitmaps {
  uint8_t *blackBitmap;
  uint8_t *yellowBitmap;
  uint8_t *redBitmap;
  uint8_t *blueBitmap;
  uint8_t *greenBitmap;
  uint32_t width;
  uint32_t height;
  size_t bitmapSize;

  ColorImageBitmaps()
      : blackBitmap(nullptr), yellowBitmap(nullptr), redBitmap(nullptr),
        blueBitmap(nullptr), greenBitmap(nullptr), width(0), height(0),
        bitmapSize(0) {}

  ~ColorImageBitmaps() {
    if (blackBitmap)
      free(blackBitmap);
    if (yellowBitmap)
      free(yellowBitmap);
    if (redBitmap)
      free(redBitmap);
    if (blueBitmap)
      free(blueBitmap);
    if (greenBitmap)
      free(greenBitmap);
  }
};

class ImageScreen : public Screen {
private:
  DisplayType &display;
  U8G2_FOR_ADAFRUIT_GFX gfx;
  ApplicationConfig &config;
  ApplicationConfigStorage &configStorage;
  HttpDownloader downloader;

  const uint8_t *smallFont;
  String ditheringServiceUrl;

  std::unique_ptr<DownloadResult> download();
  std::unique_ptr<ColorImageBitmaps> processImageData(uint8_t *data,
                                                      size_t dataSize);
  std::unique_ptr<ColorImageBitmaps> processImageFile(File &file);
  void renderBitmaps(const ColorImageBitmaps &bitmaps);
  void displayBatteryStatus();
  void displayWifiInfo();
  void storeImageETag(const String &etag);
  String getStoredImageETag();
  std::unique_ptr<ColorImageBitmaps> loadFromLittleFS();
  std::unique_ptr<ColorImageBitmaps> loadFromFolder();
  std::unique_ptr<ColorImageBitmaps>
  ditherImage(uint16_t *rgb565Buffer, uint32_t width, uint32_t height);

  std::unique_ptr<ColorImageBitmaps> decodeJPG(uint8_t *data, size_t dataSize);
  std::unique_ptr<ColorImageBitmaps> decodeJPG(const String &filename);
  std::unique_ptr<ColorImageBitmaps> decodePNG(File &file);
  std::unique_ptr<ColorImageBitmaps> decodePNG(uint8_t *data, size_t dataSize);
  std::unique_ptr<ColorImageBitmaps> decodeBMP(uint8_t *data, size_t dataSize);
  std::unique_ptr<ColorImageBitmaps> decodeSpectra6(uint8_t *data, size_t dataSize);

public:
  ImageScreen(DisplayType &display, ApplicationConfig &config,
              ApplicationConfigStorage &configStorage);
  void render() override;
  int nextRefreshInSeconds() override;
};

#endif
