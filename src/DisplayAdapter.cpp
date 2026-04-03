#include "DisplayAdapter.h"

// ============================================================
// DisplayAdapter: bridges Adafruit_GFX drawing into a PSRAM
// framebuffer, then flushes it to the 13.3" Spectra 6 panel
// via the manufacturer's QSPI driver (dual driver IC protocol).
// ============================================================

// Frame buffer size: each byte holds 2 pixels (4-bit nibbles)
static const size_t FRAME_BUFFER_SIZE =
    (EPD_NATIVE_WIDTH * EPD_NATIVE_HEIGHT) / 2; // 960000 bytes

DisplayAdapter::DisplayAdapter()
    : Adafruit_GFX(EPD_NATIVE_WIDTH, EPD_NATIVE_HEIGHT), _frameBuffer(nullptr),
      _initialized(false) {}

void DisplayAdapter::init(uint32_t serial_diag_bitrate) {
  if (!_initialized) {
    // Allocate framebuffer in PSRAM
    _frameBuffer = (uint8_t *)ps_malloc(FRAME_BUFFER_SIZE);
    if (!_frameBuffer) {
      Serial.println(
          "FATAL: Failed to allocate PSRAM framebuffer for 13.3\" display!");
      return;
    }
    // Fill with white
    memset(_frameBuffer, (WHITE << 4) | WHITE, FRAME_BUFFER_SIZE);

    // Initialize GPIO and SPI bus using the manufacturer's driver
    initialGpio();
    initialSpi();
    setGpioLevel(LOAD_SW, GPIO_HIGH);
    epdHardwareReset();
    setPinCsAll(GPIO_HIGH);

    // Send display init sequence
    initEPD();

    _initialized = true;
  } else {
    // Re-init the EPD registers (as the manufacturer does before each refresh)
    initEPD();
  }
}

void DisplayAdapter::setRotation(uint8_t r) { Adafruit_GFX::setRotation(r); }

void DisplayAdapter::setFullWindow() {
  // No-op for this driver; full-window is the default mode
}

void DisplayAdapter::fillScreen(uint16_t color) {
  if (!_frameBuffer)
    return;
  uint8_t packedColor = ((color & 0xFF) << 4) | (color & 0xFF);
  memset(_frameBuffer, packedColor, FRAME_BUFFER_SIZE);
}

void DisplayAdapter::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (!_frameBuffer)
    return;

  // Apply rotation (Adafruit_GFX gives us logical coordinates)
  int16_t t;
  switch (getRotation()) {
  case 0:
    break;
  case 1:
    t = x;
    x = EPD_NATIVE_WIDTH - 1 - y;
    y = t;
    break;
  case 2:
    x = EPD_NATIVE_WIDTH - 1 - x;
    y = EPD_NATIVE_HEIGHT - 1 - y;
    break;
  case 3:
    t = x;
    x = y;
    y = EPD_NATIVE_HEIGHT - 1 - t;
    break;
  }

  // Bounds check on physical coordinates
  if (x < 0 || x >= EPD_NATIVE_WIDTH || y < 0 || y >= EPD_NATIVE_HEIGHT)
    return;

  // Pack into the framebuffer (2 pixels per byte, high nibble = even x, low
  // nibble = odd x)
  size_t index = ((size_t)y * EPD_NATIVE_WIDTH + x) / 2;
  if (x % 2 == 0) {
    _frameBuffer[index] = (_frameBuffer[index] & 0x0F) | ((color & 0x0F) << 4);
  } else {
    _frameBuffer[index] = (_frameBuffer[index] & 0xF0) | (color & 0x0F);
  }
}

void DisplayAdapter::sendFrameBufferToDisplay() {
  if (!_frameBuffer)
    return;

  // The 13.3" display has TWO driver ICs, each handling half the width.
  // From the manufacturer's pic_display_test():
  //   Width  = EPD_NATIVE_WIDTH / 2 = 600 pixels per IC
  //   Width1 = 300 bytes per IC per row (600 pixels / 2 pixels per byte)
  //
  // CS0 (left half):  for each row, send bytes [0..Width1-1]
  // CS1 (right half): for each row, send bytes [Width1..Width-1]

  const unsigned int Width = EPD_NATIVE_WIDTH / 2; // 600 pixels per section
  const unsigned int Width1 = Width / 2; // 300 bytes per section per row
  const unsigned int Height = EPD_NATIVE_HEIGHT; // 1600 rows

  // --- Send left half (CS0) ---
  setPinCsAll(GPIO_HIGH);
  setPinCs(0, 0);
  writeEpdCommand(DTM);
  for (unsigned int row = 0; row < Height; row++) {
    writeEpdData(_frameBuffer + row * Width, Width1);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  setPinCsAll(GPIO_HIGH);

  // --- Send right half (CS1) ---
  setPinCs(1, 0);
  writeEpdCommand(DTM);
  for (unsigned int row = 0; row < Height; row++) {
    writeEpdData(_frameBuffer + row * Width + Width1, Width1);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  setPinCsAll(GPIO_HIGH);
}

void DisplayAdapter::display(bool partial_update_mode) {
  sendFrameBufferToDisplay();
  epdDisplay();
}

void DisplayAdapter::hibernate() {
  // Power off the display to save energy
  setPinCsAll(GPIO_LOW);
  writeEpd(POF, (unsigned char *)POF_V, sizeof(POF_V));
  checkBusyHigh();
  setPinCsAll(GPIO_HIGH);
}
