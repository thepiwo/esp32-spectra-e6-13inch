#include "ImageScreen.h"
#include <Arduino.h>

#include <LittleFS.h>
#include <PNGdec.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>

#include "FolderImageSource.h"
#include "battery.h"
#include <FS.h>

ImageScreen::ImageScreen(DisplayType &display, ApplicationConfig &config,
                         ApplicationConfigStorage &configStorage)
    : display(display), config(config), configStorage(configStorage),
      smallFont(u8g2_font_helvR12_tr) {
  gfx.begin(display);
}

void ImageScreen::storeImageETag(const String &etag) {
  strncpy(storedImageETag, etag.c_str(), sizeof(storedImageETag) - 1);
  storedImageETag[sizeof(storedImageETag) - 1] = '\0';
  Serial.println("Stored ETag in RTC memory: " + etag);
}

String ImageScreen::getStoredImageETag() { return String(storedImageETag); }

std::unique_ptr<DownloadResult> ImageScreen::download() {
  String storedETag = getStoredImageETag();
  Serial.println("Using stored ETag for request: '" + storedETag + "'");
  auto result = downloader.download(String(config.imageUrl), storedETag);

  if (result->etag.length() > 0) {
    storeImageETag(result->etag);
  }

  return result;
}

// Spectra 6 palette colors (RGB888)
struct RGBColor {
  uint8_t r, g, b;
};

static const RGBColor Spectra6Palette[] = {
    {0, 0, 0},       // 0: Black
    {255, 255, 255}, // 1: White
    {230, 230, 0},   // 2: Yellow (e6e600)
    {204, 0, 0},     // 3: Red (cc0000)
    {0, 51, 204},    // 4: Blue (0033cc)
    {0, 204, 0}      // 5: Green (00cc00)
};

static void scaleToFit(uint16_t *buffer, uint32_t srcW, uint32_t srcH) {
  const uint32_t dstW = 1200;
  const uint32_t dstH = 1600;

  if (srcW == dstW && srcH == dstH)
    return; // Already exact size, nothing to do

  // Calculate scale to fit entire display (contain mode - preserves aspect
  // ratio)
  float scaleX = (float)dstW / (float)srcW;
  float scaleY = (float)dstH / (float)srcH;
  float scale = (scaleX < scaleY) ? scaleX : scaleY; // Use smaller scale to fit

  uint32_t scaledW = (uint32_t)(srcW * scale);
  uint32_t scaledH = (uint32_t)(srcH * scale);
  if (scaledW > dstW)
    scaledW = dstW;
  if (scaledH > dstH)
    scaledH = dstH;

  // Center the scaled image (letterbox or pillarbox)
  uint32_t offsetX = (dstW - scaledW) / 2;
  uint32_t offsetY = (dstH - scaledH) / 2;

  Serial.printf("Scaling %dx%d -> %dx%d (scale=%.2f, offset=%d,%d)\n", srcW,
                srcH, scaledW, scaledH, scale, offsetX, offsetY);

  // 1. Horizontal Scale (In Place)
  // For each row, we horizontally scale and center it within the 1200 pixel
  // width.
  for (uint32_t y = 0; y < srcH; y++) {
    uint16_t rowBuf[1200];
    memcpy(rowBuf, &buffer[y * dstW], srcW * sizeof(uint16_t));
    for (uint32_t x = 0; x < dstW; x++)
      buffer[y * dstW + x] = 0xFFFF; // Clear back to white
    for (uint32_t dx = 0; dx < scaledW; dx++) {
      uint32_t srcX = dx * srcW / scaledW;
      if (srcX >= srcW)
        srcX = srcW - 1;
      buffer[y * dstW + offsetX + dx] = rowBuf[srcX];
    }
  }

  // 2. Vertical Scale (In Place)
  // We map srcY (0 to srcH-1) to dstY (offsetY to offsetY + scaledH - 1)
  // To avoid overwriting source rows before we read them, we analyze the
  // overlap mapping direction.
  int32_t crossPoint = -1;
  for (uint32_t dy = 0; dy < scaledH; dy++) {
    uint32_t srcY = dy * srcH / scaledH;
    uint32_t targetY = offsetY + dy;
    if (targetY < srcY) {
      crossPoint = dy;
      break; // First point where target skips above source
    }
  }

  // Bottom-up for the top half (where targetY >= srcY)
  int32_t limitBottomUp = (crossPoint == -1) ? (scaledH - 1) : (crossPoint - 1);
  for (int32_t dy = limitBottomUp; dy >= 0; dy--) {
    uint32_t srcY = dy * srcH / scaledH;
    if (srcY >= srcH)
      srcY = srcH - 1;
    uint32_t targetY = offsetY + dy;
    if (targetY != srcY) {
      memcpy(&buffer[targetY * dstW], &buffer[srcY * dstW],
             dstW * sizeof(uint16_t));
    }
  }

  // Top-down for the bottom half (where targetY < srcY)
  if (crossPoint != -1) {
    for (uint32_t dy = crossPoint; dy < scaledH; dy++) {
      uint32_t srcY = dy * srcH / scaledH;
      if (srcY >= srcH)
        srcY = srcH - 1;
      uint32_t targetY = offsetY + dy;
      if (targetY != srcY) {
        memcpy(&buffer[targetY * dstW], &buffer[srcY * dstW],
               dstW * sizeof(uint16_t));
      }
    }
  }

  // 3. Fill the vertical letterbox areas with white
  for (uint32_t y = 0; y < offsetY; y++) {
    for (uint32_t x = 0; x < dstW; x++)
      buffer[y * dstW + x] = 0xFFFF;
  }
  for (uint32_t y = offsetY + scaledH; y < dstH; y++) {
    for (uint32_t x = 0; x < dstW; x++)
      buffer[y * dstW + x] = 0xFFFF;
  }
}

static uint8_t findNearestColor(int r, int g, int b) {
  uint32_t minDistance = 0xFFFFFFFF;
  uint8_t nearestIndex = 1; // Default to white

  for (uint8_t i = 0; i < 6; i++) {
    int dr = r - Spectra6Palette[i].r;
    int dg = g - Spectra6Palette[i].g;
    int db = b - Spectra6Palette[i].b;
    uint32_t distance = dr * dr + dg * dg + db * db;
    if (distance < minDistance) {
      minDistance = distance;
      nearestIndex = i;
    }
  }
  return nearestIndex;
}

// ---- Shared helpers for all dithering algorithms ----

static std::unique_ptr<ColorImageBitmaps> allocateBitmaps(uint32_t width,
                                                          uint32_t height) {
  int bitmapWidthBytes = (width + 7) / 8;
  size_t bitmapSize = bitmapWidthBytes * height;
  auto b = std::unique_ptr<ColorImageBitmaps>(new ColorImageBitmaps());
  b->width = width;
  b->height = height;
  b->bitmapSize = bitmapSize;
  b->blackBitmap = (uint8_t *)ps_malloc(bitmapSize);
  b->yellowBitmap = (uint8_t *)ps_malloc(bitmapSize);
  b->redBitmap = (uint8_t *)ps_malloc(bitmapSize);
  b->blueBitmap = (uint8_t *)ps_malloc(bitmapSize);
  b->greenBitmap = (uint8_t *)ps_malloc(bitmapSize);
  if (!b->blackBitmap || !b->yellowBitmap || !b->redBitmap || !b->blueBitmap ||
      !b->greenBitmap) {
    Serial.println("Failed to allocate PSRAM for output bitmaps");
    return nullptr;
  }
  memset(b->blackBitmap, 0, bitmapSize);
  memset(b->yellowBitmap, 0, bitmapSize);
  memset(b->redBitmap, 0, bitmapSize);
  memset(b->blueBitmap, 0, bitmapSize);
  memset(b->greenBitmap, 0, bitmapSize);
  return b;
}

static inline void setColorBit(ColorImageBitmaps &bm, uint8_t colorIdx,
                               int byteIndex, uint8_t bitMask) {
  switch (colorIdx) {
  case 0:
    bm.blackBitmap[byteIndex] |= bitMask;
    break;
  case 2:
    bm.yellowBitmap[byteIndex] |= bitMask;
    break;
  case 3:
    bm.redBitmap[byteIndex] |= bitMask;
    break;
  case 4:
    bm.blueBitmap[byteIndex] |= bitMask;
    break;
  case 5:
    bm.greenBitmap[byteIndex] |= bitMask;
    break;
  }
}

static inline void extractRGB565(uint16_t p565, int &r, int &g, int &b) {
  r = ((p565 >> 11) & 0x1F) << 3;
  g = ((p565 >> 5) & 0x3F) << 2;
  b = (p565 & 0x1F) << 3;
}

// ---- Floyd-Steinberg dithering (default) ----

static std::unique_ptr<ColorImageBitmaps>
ditherFloydSteinberg(uint16_t *rgb565Buffer, uint32_t width, uint32_t height) {
  Serial.println("Dithering: Floyd-Steinberg");
  auto bitmaps = allocateBitmaps(width, height);
  if (!bitmaps)
    return nullptr;
  int bitmapWidthBytes = (width + 7) / 8;

  int16_t *errR_curr = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errG_curr = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errB_curr = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errR_next = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errG_next = (int16_t *)calloc(width, sizeof(int16_t));
  int16_t *errB_next = (int16_t *)calloc(width, sizeof(int16_t));

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      int r, g, b;
      extractRGB565(rgb565Buffer[y * width + x], r, g, b);
      r = constrain(r + errR_curr[x], 0, 255);
      g = constrain(g + errG_curr[x], 0, 255);
      b = constrain(b + errB_curr[x], 0, 255);

      uint8_t ci = findNearestColor(r, g, b);
      int flippedY = (height - 1) - y;
      setColorBit(*bitmaps, ci, flippedY * bitmapWidthBytes + x / 8,
                  1 << (7 - (x % 8)));

      int eR = r - Spectra6Palette[ci].r;
      int eG = g - Spectra6Palette[ci].g;
      int eB = b - Spectra6Palette[ci].b;

      if (x + 1 < width) {
        errR_curr[x + 1] += (eR * 7) / 16;
        errG_curr[x + 1] += (eG * 7) / 16;
        errB_curr[x + 1] += (eB * 7) / 16;
      }
      if (y + 1 < height) {
        if (x > 0) {
          errR_next[x - 1] += (eR * 3) / 16;
          errG_next[x - 1] += (eG * 3) / 16;
          errB_next[x - 1] += (eB * 3) / 16;
        }
        errR_next[x] += (eR * 5) / 16;
        errG_next[x] += (eG * 5) / 16;
        errB_next[x] += (eB * 5) / 16;
        if (x + 1 < width) {
          errR_next[x + 1] += (eR * 1) / 16;
          errG_next[x + 1] += (eG * 1) / 16;
          errB_next[x + 1] += (eB * 1) / 16;
        }
      }
    }
    memcpy(errR_curr, errR_next, width * sizeof(int16_t));
    memcpy(errG_curr, errG_next, width * sizeof(int16_t));
    memcpy(errB_curr, errB_next, width * sizeof(int16_t));
    memset(errR_next, 0, width * sizeof(int16_t));
    memset(errG_next, 0, width * sizeof(int16_t));
    memset(errB_next, 0, width * sizeof(int16_t));
  }
  free(errR_curr);
  free(errG_curr);
  free(errB_curr);
  free(errR_next);
  free(errG_next);
  free(errB_next);
  return bitmaps;
}

// ---- Atkinson dithering — diffuses only 6/8 of error for higher contrast ----

static std::unique_ptr<ColorImageBitmaps>
ditherAtkinson(uint16_t *rgb565Buffer, uint32_t width, uint32_t height) {
  Serial.println("Dithering: Atkinson");
  auto bitmaps = allocateBitmaps(width, height);
  if (!bitmaps)
    return nullptr;
  int bitmapWidthBytes = (width + 7) / 8;

  // Need 3 rows of error: current, next, next+1
  int16_t *eR[3], *eG[3], *eB[3];
  for (int i = 0; i < 3; i++) {
    eR[i] = (int16_t *)calloc(width, sizeof(int16_t));
    eG[i] = (int16_t *)calloc(width, sizeof(int16_t));
    eB[i] = (int16_t *)calloc(width, sizeof(int16_t));
  }

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      int r, g, b;
      extractRGB565(rgb565Buffer[y * width + x], r, g, b);
      r = constrain(r + eR[0][x], 0, 255);
      g = constrain(g + eG[0][x], 0, 255);
      b = constrain(b + eB[0][x], 0, 255);

      uint8_t ci = findNearestColor(r, g, b);
      int flippedY = (height - 1) - y;
      setColorBit(*bitmaps, ci, flippedY * bitmapWidthBytes + x / 8,
                  1 << (7 - (x % 8)));

      // Atkinson: distribute 6/8 of error (lose 2/8 = sharper contrast)
      int dR = (r - Spectra6Palette[ci].r) / 8;
      int dG = (g - Spectra6Palette[ci].g) / 8;
      int dB = (b - Spectra6Palette[ci].b) / 8;

      // Right +1, Right +2
      if (x + 1 < width) {
        eR[0][x + 1] += dR;
        eG[0][x + 1] += dG;
        eB[0][x + 1] += dB;
      }
      if (x + 2 < width) {
        eR[0][x + 2] += dR;
        eG[0][x + 2] += dG;
        eB[0][x + 2] += dB;
      }
      // Next row: left, center, right
      if (y + 1 < height) {
        if (x > 0) {
          eR[1][x - 1] += dR;
          eG[1][x - 1] += dG;
          eB[1][x - 1] += dB;
        }
        eR[1][x] += dR;
        eG[1][x] += dG;
        eB[1][x] += dB;
        if (x + 1 < width) {
          eR[1][x + 1] += dR;
          eG[1][x + 1] += dG;
          eB[1][x + 1] += dB;
        }
      }
      // Two rows down: center
      if (y + 2 < height) {
        eR[2][x] += dR;
        eG[2][x] += dG;
        eB[2][x] += dB;
      }
    }
    // Rotate error rows
    int16_t *tmpR = eR[0], *tmpG = eG[0], *tmpB = eB[0];
    eR[0] = eR[1];
    eG[0] = eG[1];
    eB[0] = eB[1];
    eR[1] = eR[2];
    eG[1] = eG[2];
    eB[1] = eB[2];
    eR[2] = tmpR;
    eG[2] = tmpG;
    eB[2] = tmpB;
    memset(eR[2], 0, width * sizeof(int16_t));
    memset(eG[2], 0, width * sizeof(int16_t));
    memset(eB[2], 0, width * sizeof(int16_t));
  }
  for (int i = 0; i < 3; i++) {
    free(eR[i]);
    free(eG[i]);
    free(eB[i]);
  }
  return bitmaps;
}

// ---- Ordered (Bayer 8x8) dithering — no error diffusion ----

static const uint8_t bayer8x8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};

static std::unique_ptr<ColorImageBitmaps>
ditherOrdered(uint16_t *rgb565Buffer, uint32_t width, uint32_t height) {
  Serial.println("Dithering: Ordered (Bayer 8x8)");
  auto bitmaps = allocateBitmaps(width, height);
  if (!bitmaps)
    return nullptr;
  int bitmapWidthBytes = (width + 7) / 8;

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      int r, g, b;
      extractRGB565(rgb565Buffer[y * width + x], r, g, b);

      // Apply Bayer threshold bias (-32..+31 range mapped to -64..+63)
      int bias = ((int)bayer8x8[y & 7][x & 7] - 32) * 2;
      r = constrain(r + bias, 0, 255);
      g = constrain(g + bias, 0, 255);
      b = constrain(b + bias, 0, 255);

      uint8_t ci = findNearestColor(r, g, b);
      int flippedY = (height - 1) - y;
      setColorBit(*bitmaps, ci, flippedY * bitmapWidthBytes + x / 8,
                  1 << (7 - (x % 8)));
    }
  }
  return bitmaps;
}

// ---- No dithering — nearest colour only ----

static std::unique_ptr<ColorImageBitmaps>
ditherNone(uint16_t *rgb565Buffer, uint32_t width, uint32_t height) {
  Serial.println("Dithering: None (nearest colour)");
  auto bitmaps = allocateBitmaps(width, height);
  if (!bitmaps)
    return nullptr;
  int bitmapWidthBytes = (width + 7) / 8;

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      int r, g, b;
      extractRGB565(rgb565Buffer[y * width + x], r, g, b);
      uint8_t ci = findNearestColor(r, g, b);
      int flippedY = (height - 1) - y;
      setColorBit(*bitmaps, ci, flippedY * bitmapWidthBytes + x / 8,
                  1 << (7 - (x % 8)));
    }
  }
  return bitmaps;
}

// ---- Dispatcher — picks algorithm based on config ----

std::unique_ptr<ColorImageBitmaps>
ImageScreen::ditherImage(uint16_t *rgb565Buffer, uint32_t width,
                         uint32_t height) {
  switch (config.ditherMode) {
  case DITHER_ATKINSON:
    return ditherAtkinson(rgb565Buffer, width, height);
  case DITHER_ORDERED:
    return ditherOrdered(rgb565Buffer, width, height);
  case DITHER_NONE:
    return ditherNone(rgb565Buffer, width, height);
  default:
    return ditherFloydSteinberg(rgb565Buffer, width, height);
  }
}

static uint16_t *jpgRgb565Buffer = nullptr;
static uint32_t jpgWidth = 0;

bool ImageScreen::jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h,
                            uint16_t *bitmap) {
  if (y >= 1600 || x >= 1200)
    return true;

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int curX = x + i;
      int curY = y + j;
      if (curX < 1200 && curY < 1600) {
        size_t idx = curY * 1200 + curX;
        jpgRgb565Buffer[idx] = bitmap[j * w + i];
      }
    }
  }
  return true;
}

std::unique_ptr<ColorImageBitmaps>
ImageScreen::decodeJPG(uint8_t *data, size_t dataSize,
                       uint8_t **freeAfterDecode) {
  Serial.println("Decoding JPEG...");
  jpgRgb565Buffer = (uint16_t *)ps_malloc(1200 * 1600 * 2);
  if (!jpgRgb565Buffer) {
    Serial.println("Failed to allocate PSRAM for JPEG RGB565 buffer");
    return nullptr;
  }
  memset(jpgRgb565Buffer, 0xFFFF, 1200 * 1600 * 2); // White background

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpgOutput);

  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, data, dataSize);

  // Calculate scale factor to reduce PSRAM allocation for huge images
  // TJpgDec only supports scaling down by 1, 2, 4, or 8.
  uint8_t scale = 1;
  while ((w / scale > 1200) || (h / scale > 1600)) {
    if (scale == 8)
      break;
    scale *= 2;
  }
  TJpgDec.setJpgScale(scale);
  Serial.printf("JPEG Original Size: %dx%d, Pre-scaling: 1/%d\n", w, h, scale);
  w /= scale;
  h /= scale;

  if (TJpgDec.drawJpg(0, 0, data, dataSize) != 0) {
    Serial.println("JPEG decode failed");
    free(jpgRgb565Buffer);
    return nullptr;
  }

  // Free source data NOW to reclaim PSRAM before dithering
  if (freeAfterDecode && *freeAfterDecode) {
    Serial.printf("Freeing source buffer to reclaim PSRAM\n");
    free(*freeAfterDecode);
    *freeAfterDecode = nullptr;
  }

  // Scale all images that don't exactly match the display to fit
  if (w != 1200 || h != 1600) {
    scaleToFit(jpgRgb565Buffer, w, h);
  }

  auto bitmaps = ditherImage(jpgRgb565Buffer, 1200, 1600);
  free(jpgRgb565Buffer);
  return bitmaps;
}

static uint16_t *pngRgb565Buffer = nullptr;

static int pngDrawCallback(PNGDRAW *pDraw) {
  // Bounds check: skip lines outside our 1200x1600 framebuffer
  if (pDraw->y >= 1600)
    return 1;

  // Use a temp buffer for the full PNG row (may be wider than 1200)
  // Then copy only the first 1200 pixels (or fewer) to our framebuffer
  int srcWidth = pDraw->iWidth;
  int copyWidth = (srcWidth > 1200) ? 1200 : srcWidth;

  // Allocate temp buffer on stack for one row (max 2400px = 4800 bytes)
  uint16_t tempLine[2400];
  PNG *png = (PNG *)pDraw->pUser;
  png->getLineAsRGB565(pDraw, tempLine, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);

  // Copy only the cropped width into our framebuffer
  memcpy(&pngRgb565Buffer[pDraw->y * 1200], tempLine,
         copyWidth * sizeof(uint16_t));
  return 1;
}

static void *pngOpenCallback(const char *filename, int32_t *size) {
  File *file = (File *)filename;
  *size = file->size();
  return (void *)file;
}

static void pngCloseCallback(void *handle) {
  // File must remain open for ImageScreen to process it or close it, so do
  // nothing here
}

static int32_t pngReadCallback(PNGFILE *hFile, uint8_t *pBuf, int32_t iLen) {
  File *file = (File *)hFile->fHandle;
  return file->read(pBuf, iLen);
}

static int32_t pngSeekCallback(PNGFILE *hFile, int32_t iPosition) {
  File *file = (File *)hFile->fHandle;
  return file->seek(iPosition) ? iPosition : 0;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodePNG(File &file) {
  Serial.println("Decoding PNG (Streaming from LittleFS)...");

  // Ensure file is at position 0
  file.seek(0);
  Serial.printf("LittleFS file size: %d, position: %d\n", file.size(),
                file.position());

  pngRgb565Buffer = (uint16_t *)ps_malloc(1200 * 1600 * 2);
  if (!pngRgb565Buffer) {
    Serial.println("Failed to allocate PSRAM for PNG RGB565 buffer");
    return nullptr;
  }
  memset(pngRgb565Buffer, 0xFFFF, 1200 * 1600 * 2);

  // Allocate PNG state on the heap to prevent stack overflow! (PNG state is
  // very large)
  PNG *png = new PNG();

  int rc = png->open((const char *)&file, pngOpenCallback, pngCloseCallback,
                     pngReadCallback, pngSeekCallback, pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG open failed (rc=%d, err=%d)\n", rc, png->getLastError());
    free(pngRgb565Buffer);
    pngRgb565Buffer = nullptr;
    delete png;
    return nullptr;
  }

  int imgW = png->getWidth();
  int imgH = png->getHeight();
  int imgType = png->getPixelType();
  Serial.printf("PNG Size: %dx%d, Type: %d, BPP: %d, Alpha: %d\n", imgW, imgH,
                imgType, png->getBpp(), png->hasAlpha());

  // Safety: check if image is too wide for our buffer
  if (imgW > 1200 || imgH > 1600) {
    Serial.printf("WARNING: PNG dimensions %dx%d exceed 1200x1600 buffer!\n",
                  imgW, imgH);
  }

  rc = png->decode((void *)png, 0);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG decode failed (rc=%d, err=%d)\n", rc,
                  png->getLastError());
    free(pngRgb565Buffer);
    pngRgb565Buffer = nullptr;
    delete png;
    return nullptr;
  }

  // Scale all images that don't exactly match the display to fit
  if (imgW != 1200 || imgH != 1600) {
    scaleToFit(pngRgb565Buffer, imgW, imgH);
  }

  auto bitmaps = ditherImage(pngRgb565Buffer, 1200, 1600);
  free(pngRgb565Buffer);
  pngRgb565Buffer = nullptr;
  delete png;
  return bitmaps;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodePNG(uint8_t *data,
                                                          size_t dataSize) {
  Serial.println("Decoding PNG (RAM)...");
  pngRgb565Buffer = (uint16_t *)ps_malloc(1200 * 1600 * 2);
  if (!pngRgb565Buffer) {
    Serial.println("Failed to allocate PSRAM for PNG RGB565 buffer");
    return nullptr;
  }
  memset(pngRgb565Buffer, 0xFFFF, 1200 * 1600 * 2);

  PNG *png = new PNG();

  int rc = png->openRAM(data, dataSize, pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    Serial.println("PNG open failed");
    free(pngRgb565Buffer);
    delete png;
    return nullptr;
  }

  Serial.printf("PNG Size: %dx%d, Type: %d\n", png->getWidth(),
                png->getHeight(), png->getPixelType());

  rc = png->decode((void *)png, 0);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG decode failed (RAM) (rc=%d, err=%d)\n", rc,
                  png->getLastError());
    free(pngRgb565Buffer);
    pngRgb565Buffer = nullptr;
    delete png;
    return nullptr;
  }

  int pngW = png->getWidth();
  int pngH = png->getHeight();

  // Scale all images that don't exactly match the display to fit
  if (pngW != 1200 || pngH != 1600) {
    scaleToFit(pngRgb565Buffer, pngW, pngH);
  }

  auto bitmaps = ditherImage(pngRgb565Buffer, 1200, 1600);
  free(pngRgb565Buffer);
  pngRgb565Buffer = nullptr;
  delete png;
  return bitmaps;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::decodeBMP(uint8_t *data,
                                                          size_t dataSize) {
  size_t dataIndex = 0;

  if (dataSize < 54) {
    Serial.printf("Payload too small for BMP header: got %d bytes, expected at "
                  "least 54\n",
                  dataSize);
    return nullptr;
  }

  uint8_t bmpHeader[54];
  memcpy(bmpHeader, data + dataIndex, 54);
  dataIndex += 54;

  if (bmpHeader[0] != 'B' || bmpHeader[1] != 'M') {
    return nullptr;
  }

  uint32_t dataOffset = bmpHeader[10] | (bmpHeader[11] << 8) |
                        (bmpHeader[12] << 16) | (bmpHeader[13] << 24);
  uint32_t imageWidth = bmpHeader[18] | (bmpHeader[19] << 8) |
                        (bmpHeader[20] << 16) | (bmpHeader[21] << 24);
  uint32_t imageHeight = bmpHeader[22] | (bmpHeader[23] << 8) |
                         (bmpHeader[24] << 16) | (bmpHeader[25] << 24);
  uint16_t bitsPerPixel = bmpHeader[28] | (bmpHeader[29] << 8);
  uint32_t compression = bmpHeader[30] | (bmpHeader[31] << 8) |
                         (bmpHeader[32] << 16) | (bmpHeader[33] << 24);

  // If it's a standard 8-bit BMP (like from's dither tool), we handle it
  // specifically
  if (bitsPerPixel == 8 && compression == 0) {
    // (Old logic for pre-dithered BMPs)
    uint32_t paletteSize = 256 * 4;
    dataIndex += paletteSize;
    if (dataOffset > dataIndex)
      dataIndex += (dataOffset - dataIndex);

    uint32_t rowSize = ((imageWidth * bitsPerPixel + 31) / 32) * 4;
    uint8_t *rowBuffer = new uint8_t[rowSize];
    uint8_t *pixelBuffer = (uint8_t *)ps_malloc(imageWidth * imageHeight);

    for (int y = imageHeight - 1; y >= 0; y--) {
      memcpy(rowBuffer, data + dataIndex, rowSize);
      dataIndex += rowSize;
      for (int x = 0; x < imageWidth; x++) {
        pixelBuffer[((imageHeight - 1) - y) * imageWidth + x] = rowBuffer[x];
      }
    }
    delete[] rowBuffer;

    // Convert mapping logic... wait, if it's already dithered to indices 0-5,
    // we can just map them.
    // But for native BMP support (random 24bit BMP), we should decode to
    // RGB888.
  }

  // FALLBACK: For non-8bit BMP or generic BMP, we should ideally decode to
  // RGB888. For now, let's assume if it starts with 'BM' and isn't our special
  // 8bit, we might need a BMP library or just simple 24bit parsing.

  if (bitsPerPixel == 24) {
    Serial.println("Decoding 24-bit BMP...");
    uint32_t rowSize = ((imageWidth * 24 + 31) / 32) * 4;
    uint16_t *rgb565Buffer = (uint16_t *)ps_malloc(1200 * 1600 * 2);
    if (!rgb565Buffer)
      return nullptr;
    memset(rgb565Buffer, 0xFFFF, 1200 * 1600 * 2); // White background

    dataIndex = dataOffset;
    for (int y = imageHeight - 1; y >= 0; y--) {
      uint16_t *pOut = rgb565Buffer + (y * 1200);
      for (int x = 0; x < imageWidth; x++) {
        if (x >= 1200 || y >= 1600) {
          dataIndex += 3;
          continue;
        }
        // BMP is BGR
        uint8_t b = data[dataIndex++];
        uint8_t g = data[dataIndex++];
        uint8_t r = data[dataIndex++];

        // Convert to RGB565
        uint16_t r5 = (r >> 3) & 0x1F;
        uint16_t g6 = (g >> 2) & 0x3F;
        uint16_t b5 = (b >> 3) & 0x1F;
        pOut[x] = (r5 << 11) | (g6 << 5) | b5;
      }
      // Skip padding
      dataIndex += (rowSize - imageWidth * 3);
    }
    auto bitmaps = ditherImage(rgb565Buffer, 1200, 1600);
    free(rgb565Buffer);
    return bitmaps;
  }

  return nullptr; // Unsupported BMP format
}

std::unique_ptr<ColorImageBitmaps>
ImageScreen::processImageData(uint8_t *data, size_t dataSize,
                              uint8_t **freeAfterDecode) {
  if (dataSize < 4)
    return nullptr;

  // Manual format detection
  if (data[0] == 0xFF && data[1] == 0xD8) {
    return decodeJPG(data, dataSize, freeAfterDecode);
  } else if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
             data[3] == 'G') {
    return decodePNG(data, dataSize);
  } else if (data[0] == 'B' && data[1] == 'M') {
    return decodeBMP(data, dataSize);
  }

  Serial.println("Unknown image format");
  return nullptr;
}

void ImageScreen::renderBitmaps(const ColorImageBitmaps &bitmaps) {
  // Calculate position to center the image on display
  int displayWidth = display.width();
  int displayHeight = display.height();

  // For same-size image, just position at origin
  int imageX = 0;
  int imageY = 0;

  // Only center if image is smaller than display
  if ((int)bitmaps.width < displayWidth) {
    imageX = (displayWidth - (int)bitmaps.width) / 2;
  }
  if ((int)bitmaps.height < displayHeight) {
    imageY = (displayHeight - (int)bitmaps.height) / 2;
  }

  // Ensure coordinates are valid
  imageX = max(0, imageX);
  imageY = max(0, imageY);

  // Draw all color bitmaps directly
  display.drawBitmap(imageX, imageY, bitmaps.blackBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_BLACK);
  display.drawBitmap(imageX, imageY, bitmaps.yellowBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_YELLOW);
  display.drawBitmap(imageX, imageY, bitmaps.redBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_RED);
  display.drawBitmap(imageX, imageY, bitmaps.blueBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_BLUE);
  display.drawBitmap(imageX, imageY, bitmaps.greenBitmap, bitmaps.width,
                     bitmaps.height, GxEPD_GREEN);
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::loadFromLittleFS() {
  const char *extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ""};
  String baseName = "/local_image";
  String filename = "";

  for (const char *ext : extensions) {
    if (LittleFS.exists(baseName + ext)) {
      filename = baseName + ext;
      break;
    }
  }

  if (filename == "") {
    printf("No local image found on LittleFS.\r\n");
    return nullptr;
  }

  File file = LittleFS.open(filename, FILE_READ);
  if (!file) {
    printf("Failed to open %s for reading.\r\n", filename.c_str());
    return nullptr;
  }

  if (filename.endsWith(".png")) {
    printf("Streaming PNG image directly from LittleFS to "
           "processImageData...\r\n");
    auto bitmaps = decodePNG(file);
    file.close();
    return bitmaps;
  }

  size_t fileSize = file.size();
  printf("Found %s (Size: %d bytes). Loading into PSRAM...\r\n",
         filename.c_str(), fileSize);

  uint8_t *fileBuffer = (uint8_t *)ps_malloc(fileSize);
  if (!fileBuffer) {
    printf("Failed to allocate %d bytes in PSRAM for LittleFS image.\r\n",
           fileSize);
    file.close();
    return nullptr;
  }

  size_t bytesRead = file.read(fileBuffer, fileSize);
  file.close();

  if (bytesRead != fileSize) {
    printf("Warning: Read %d bytes, expected %d bytes\r\n", bytesRead,
           fileSize);
  }

  printf("Passing local LittleFS image to processImageData...\r\n");
  auto bitmaps = processImageData(fileBuffer, bytesRead, &fileBuffer);

  // fileBuffer may have been freed by decodeJPG already
  if (fileBuffer) {
    free(fileBuffer);
  }

  return bitmaps;
}

std::unique_ptr<ColorImageBitmaps> ImageScreen::loadFromFolder() {
  uint16_t imageIndex = configStorage.loadImageIndex();
  uint16_t totalImages = 0;

  FolderImageSource folderSource;
  auto result = folderSource.fetchImage(String(config.folderUrl), imageIndex,
                                        totalImages);

  if (!result || result->httpCode != HTTP_CODE_OK || !result->data) {
    printf("Folder: failed to fetch image\r\n");
    return nullptr;
  }

  printf("Folder: processing image (%d bytes)\r\n", result->size);
  return processImageData(result->data, result->size, &result->data);
}

void ImageScreen::render() {
  display.init(115200);
  display.setRotation(ApplicationConfig::DISPLAY_ROTATION);
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);

  LittleFS.begin(true);

  // Priority 1: Local uploaded image (from web upload or SD card copy)
  auto bitmaps = loadFromLittleFS();

  // Priority 2: Folder URL — cycle through images in order
  if (!bitmaps && config.hasFolderUrl()) {
    bitmaps = loadFromFolder();
    if (!bitmaps) {
      printf("Folder source failed, falling back to single image URL\r\n");
    }
  }

  // Priority 3: Single image URL
  if (!bitmaps && strlen(config.imageUrl) > 0) {
    auto downloadResult = download();

    if (downloadResult->httpCode == HTTP_CODE_NOT_MODIFIED) {
      Serial.println("Image not modified (304), using cached version");
      return;
    }

    if (downloadResult->httpCode == HTTP_CODE_OK) {
      bitmaps = processImageData(downloadResult->data, downloadResult->size);
    } else {
      printf("Failed to download image (HTTP %d)\r\n",
             downloadResult->httpCode);
    }
  }

  if (!bitmaps) {
    printf("No image available from any source\r\n");
    return;
  }

  renderBitmaps(*bitmaps);
  displayBatteryStatus();
  displayWifiInfo();

  display.display();
  display.hibernate();
}

void ImageScreen::displayBatteryStatus() {
  String batteryStatus = getBatteryStatus();
  gfx.setFontMode(0);
  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);
  gfx.setFont(u8g2_font_helvB08_tr);

  int textWidth = gfx.getUTF8Width(batteryStatus.c_str());
  int textHeight = gfx.getFontAscent() - gfx.getFontDescent();
  int fontAscent = gfx.getFontAscent();

  int paddingX = 6;
  int paddingY = 4;
  int rectWidth = textWidth + (2 * paddingX);
  int rectHeight = textHeight + (2 * paddingY);

  int rectX = display.width() - rectWidth - 18;
  int rectY = display.height() - rectHeight - 4;

  // Calculate centered text position within the rectangle
  int batteryX = rectX + (rectWidth - textWidth) / 2;
  int batteryY = rectY + rectHeight / 2 + fontAscent / 2;

  // Draw white rounded rectangle background
  display.fillRoundRect(rectX, rectY, rectWidth, rectHeight, 4, GxEPD_WHITE);

  gfx.setCursor(batteryX, batteryY);
  gfx.print(batteryStatus);
}

void ImageScreen::displayWifiInfo() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  String wifiInfo =
      String(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ")";

  gfx.setFontMode(0);
  gfx.setBackgroundColor(GxEPD_WHITE);
  gfx.setForegroundColor(GxEPD_BLACK);
  gfx.setFont(u8g2_font_helvB08_tr);

  int textWidth = gfx.getUTF8Width(wifiInfo.c_str());
  int textHeight = gfx.getFontAscent() - gfx.getFontDescent();
  int fontAscent = gfx.getFontAscent();

  int paddingX = 6;
  int paddingY = 4;
  int rectWidth = textWidth + (2 * paddingX);
  int rectHeight = textHeight + (2 * paddingY);

  // Position at the bottom left
  int rectX = 4;
  int rectY = display.height() - rectHeight - 4;

  int infoX = rectX + paddingX;
  int infoY = rectY + rectHeight / 2 + fontAscent / 2;

  // Draw white rounded rectangle background
  display.fillRoundRect(rectX, rectY, rectWidth, rectHeight, 4, GxEPD_WHITE);

  gfx.setCursor(infoX, infoY);
  gfx.print(wifiInfo);
}

int ImageScreen::nextRefreshInSeconds() { return 1800; }
