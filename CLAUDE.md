# CLAUDE.md — ESP32-133C02 Spectra 6 E-Ink Firmware

This file is the authoritative guide for AI assistants working in this repository. Read it before touching any code.

---

## Project Summary

Firmware for the **Good-Display 13.3" E-Ink Spectra 6 panel (GDEP133C02)** driven by an **ESP32-S3** (N16R8 — 16 MB Flash, 8 MB PSRAM). The device displays full-colour (6-colour) images sourced from local uploads, an HTTP image URL, or an HTTP folder. After a 10-minute web server window it enters deep sleep, waking on a configurable timer to cycle images.

Language: **C++ (Arduino framework via PlatformIO)**. No RTOS, no IDF tasks — single-threaded `setup()` / `loop()` (loop is empty; all logic is in `setup()`).

---

## Build System

**PlatformIO** — do not use raw `idf.py` or `cmake`.

```bash
# Build only
pio run

# Build and flash firmware
pio run --target upload

# Upload LittleFS filesystem (the config.html web portal)
pio run --target uploadfs

# Serial monitor
pio device monitor -b 115200

# Build with verbose output
pio run -v
```

There are **no unit tests** in this project. Validation is done by flashing the device and reading the serial monitor.

---

## Repository Layout

```
esp32-spectra-e6-13inch/
├── src/                        # All firmware C++/C source
│   ├── main.cpp                # Entry point — full boot/sleep state machine
│   ├── ApplicationConfig.h     # Runtime config struct + DitherMode enum
│   ├── ApplicationConfigStorage.cpp/.h  # NVS read/write for config + img_index
│   ├── ConfigurationServer.cpp/.h       # ESPAsyncWebServer: portal, upload, /save
│   ├── ConfigurationScreen.cpp/.h       # AP mode display (QR code + info)
│   ├── DisplayAdapter.cpp/.h            # Adafruit_GFX subclass over QSPI driver
│   ├── ImageScreen.cpp/.h               # Decode, scale, dither, render pipeline
│   ├── FolderImageSource.cpp/.h         # HTTP directory listing + image download
│   ├── HttpDownloader.cpp/.h            # HTTPClient wrapper (chunked + ETag)
│   ├── WiFiConnection.cpp/.h            # WiFi STA connection manager
│   ├── SDCardManager.cpp/.h             # SD→LittleFS copy (runs before display init)
│   ├── battery.cpp/.h                   # ADC battery voltage (currently disabled)
│   ├── Screen.h                         # Abstract base: render() + nextRefreshInSeconds()
│   ├── GDEP133C02.c/.h                  # [Manufacturer] EPD init, commands, refresh
│   ├── comm.c/.h                        # [Manufacturer] SPI bus init, GPIO, transactions
│   ├── pindefine.h                      # [Manufacturer] GPIO pin definitions
│   ├── status.h                         # [Manufacturer] debug flag
│   ├── config_default.h                 # Empty default credentials (safe to commit)
│   └── config_dev.h                     # ← NOT committed; your local WiFi credentials
├── data/
│   └── config.html             # Web portal HTML uploaded to LittleFS (uploadfs)
├── include/
│   └── boards.h                # Board-level definitions (currently unused in firmware)
├── platformio.ini              # Build config, library deps, partition table ref
├── partitions.csv              # Custom flash layout (5.6 MB LittleFS)
├── plan.md                     # Feature design notes (architectural reference)
└── README.md                   # User-facing documentation
```

---

## Key Configurations (`platformio.ini`)

| Setting | Value | Why |
|---|---|---|
| `board` | `esp32-s3-devkitc-1` | ESP32-133C02 uses ESP32-S3 |
| `board_build.psram` | `enabled` | 8 MB PSRAM is required |
| `board_build.arduino.memory_type` | `qio_opi` | Octal PSRAM mode for ESP32-S3 |
| `board_build.filesystem` | `littlefs` | LittleFS for `uploadfs` target |
| `-DPNG_MAX_BUFFERED_PIXELS=19202` | build flag | PNGdec buffer limit for 1200-px-wide images |

---

## ApplicationConfig Fields

Defined in `src/ApplicationConfig.h`. All fields are persisted to NVS via `ApplicationConfigStorage`.

| Field | Type | Default | Purpose |
|---|---|---|---|
| `wifiSSID[64]` | char[] | `""` | WiFi network name |
| `wifiPassword[64]` | char[] | `""` | WiFi password |
| `imageUrl[300]` | char[] | `""` | Single HTTP image URL (fallback) |
| `folderUrl[300]` | char[] | `""` | HTTP folder URL for image cycling |
| `pinnedImageUrl[300]` | char[] | `""` | Pinned image URL (overrides cycling) |
| `ditherMode` | `uint8_t` | `0` | `DitherMode` enum (0=Floyd-Steinberg, 1=Atkinson, 2=Ordered/Bayer, 3=None) |
| `sleepMinutes` | `uint16_t` | `0` | Deep sleep duration (0 = permanent sleep after server window) |
| `imageChangeMinutes` | `uint16_t` | `30` | How often to advance the folder image index |

`img_index` (the folder cycling counter) is stored separately in NVS under a dedicated key — not inside the config blob — because it changes every wake cycle.

---

## Boot State Machine (`main.cpp`)

All logic runs in `setup()`. `loop()` is empty.

```
Power-on / timer wake
  │
  ├── Load config from NVS
  ├── Detect timer wake (ESP_SLEEP_WAKEUP_TIMER)
  │     └── If timer wake + folder configured + not pinned:
  │           advance img_index, reset wakesSinceImageChange if interval elapsed
  │
  ├── copyImageFromSDToLittleFS()  ← MUST be before display init (shared SPI)
  │
  ├── WiFi connect (if credentials exist)
  │
  ├── displayCurrentScreen()       ← renders image or config screen
  │
  ├── ConfigurationServer (10-min window)   ← skipped on timer wakes
  │     • /            → serve config.html
  │     • /save        → update NVS config
  │     • /upload      → store image to LittleFS, trigger display refresh
  │     • /folder-images → return JSON list of images in folder
  │     • /pin         → pin a specific folder image URL
  │
  └── esp_deep_sleep_start()
        • Timer wakeup if sleepMinutes > 0
        • Permanent sleep if sleepMinutes == 0
```

**Web server is skipped on timer wakes** (`timerWake == true`) to enable fast sleep cycling.

---

## Image Priority Chain (`ImageScreen::render`)

1. **LittleFS local image** — uploaded via web portal (highest priority)
2. **HTTP folder** — if `folderUrl` is set (uses `FolderImageSource`)
   - If `pinnedImageUrl` is set, fetches that specific URL instead of cycling
3. **Single image URL** — if `imageUrl` is set (fallback download)
4. **Nothing** — shows an info/status screen

---

## Image Processing Pipeline

```
Raw file (JPEG / PNG / BMP)
  │
  ├── JPEG: TJpg_Decoder → RGB565 buffer in PSRAM
  ├── PNG:  PNGdec (streaming from LittleFS or memory)
  └── BMP:  Manual 24-bit parser → RGB565
  │
  ▼
scaleToFit() — nearest-neighbour, aspect-ratio-preserving, in-place
  (white letterbox/pillarbox bars added for mismatched aspect ratios)
  │
  ▼
ditherImage() — dispatches on config.ditherMode:
  0: Floyd-Steinberg (default, smooth)
  1: Atkinson (higher contrast, good for e-ink)
  2: Ordered / Bayer 8×8 (structured, no error diffusion)
  3: None / Nearest colour (hard blocks, best for vector art)
  │
  ▼
5 × 1-bit bitmaps (black, yellow, red, blue, green — white = no bits set)
  │
  ▼
DisplayAdapter::drawBitmap() → 4-bit PSRAM framebuffer
  │
  ▼
sendFrameBufferToDisplay() — QSPI dual-IC split (left 600px → CS0, right 600px → CS1)
```

---

## PSRAM Memory Layout

The 8 MB PSRAM is the critical resource. Always account for these buffers:

| Buffer | Size | Notes |
|---|---|---|
| Display framebuffer | 960 KB | 1200×1600 @ 4bpp (2px/byte) |
| RGB565 decode buffer | ~3.84 MB | 1200×1600×2 bytes |
| Dither bitmaps (×5 colours) | ~1.2 MB | 1bpp per colour |
| Source image data | Variable | **Freed immediately after decode** |

**Critical rule:** For large JPEGs, the raw download buffer is freed **before** dithering begins. If you allocate new PSRAM buffers, verify the total stays under 8 MB. Use `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` to check.

---

## Hardware Constraints

### Shared SPI Bus (SD card + Display)

The SD card and e-ink display **share the same SPI3_HOST pins**:

| GPIO | Display | SD Card |
|---|---|---|
| 9 | `SPI_CLK` | SD_CLK |
| 41 | `SPI_Data0` | SD_CMD (MOSI) |
| 40 | `SPI_Data1` | SD_D0 (MISO) |
| 21 | `SW_4` | SD_CS |

**Rule:** Always call `copyImageFromSDToLittleFS()` before any display initialisation. The SD card is accessed first, then the SPI bus is fully released for the display driver.

### Display QSPI Pins

| GPIO | Function |
|---|---|
| 9 | SPI_CLK |
| 41 | SPI_Data0 |
| 40 | SPI_Data1 |
| 39 | SPI_Data2 |
| 38 | SPI_Data3 |
| 18 | CS0 (left half, IC 0) |
| 17 | CS1 (right half, IC 1) |
| 7 | EPD_BUSY (input) |
| 6 | EPD_RST |
| 45 | LOAD_SW |

### Display Dimensions

- Native: **1200 × 1600 pixels**, portrait, 3:4 aspect ratio
- `DISPLAY_ROTATION = 2` is set in `ApplicationConfig` (180° rotation for physical mounting)
- Optimal source image: **1200×1600 px**; any other size is scaled automatically

---

## DisplayAdapter

`DisplayAdapter` inherits from `Adafruit_GFX` and wraps the manufacturer's C driver (`GDEP133C02.c` / `comm.c`).

- **Framebuffer:** 960 KB in PSRAM (allocated via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`)
- **Colour codes:** 4-bit values matching manufacturer constants (`BLACK=0x00`, `WHITE=0x11`, `RED=0x33`, etc.)
- **`display()`:** Sends the framebuffer to the panel via QSPI; left 600 columns → CS0, right 600 columns → CS1
- **Refresh time:** ~20 seconds for a full screen update

The manufacturer C files (`GDEP133C02.c`, `comm.c`, `pindefine.h`, `status.h`) are wrapped with `extern "C"` in `DisplayAdapter.h` and should not be modified unless adapting to a different panel variant.

---

## NVS Storage

Managed by `ApplicationConfigStorage`. Uses the `Preferences` library internally.

| NVS Namespace | Key | Content |
|---|---|---|
| `(configured in .cpp)` | `CONFIG_KEY` | Serialised `ApplicationConfig` blob |
| `(configured in .cpp)` | `IMG_INDEX_KEY` | `uint16_t` folder cycling index |

The `img_index` is stored separately so it can be incremented on every timer wake without rewriting the full config blob.

---

## ConfigurationServer (Web Portal)

Built on `ESPAsyncWebServer`. The HTML template (`data/config.html`) is served from LittleFS.

Template placeholder variables injected at render time:

| Placeholder | Field |
|---|---|
| `{{CURRENT_SSID}}` | `wifiSSID` |
| `{{CURRENT_IMAGE_URL}}` | `imageUrl` |
| `{{CURRENT_FOLDER_URL}}` | `folderUrl` |
| `{{CURRENT_PINNED_URL}}` | `pinnedImageUrl` |
| `{{CURRENT_DITHER_MODE}}` | `ditherMode` |
| `{{CURRENT_SLEEP_MINUTES}}` | `sleepMinutes` |
| `{{CURRENT_IMAGE_CHANGE_MINUTES}}` | `imageChangeMinutes` |

API endpoints:

| Path | Method | Purpose |
|---|---|---|
| `/` | GET | Serve config portal HTML |
| `/save` | POST | Save WiFi/URL/dithering/sleep settings |
| `/upload` | POST (multipart) | Upload image to LittleFS |
| `/folder-images` | GET | Return JSON array of images in folder |
| `/pin` | POST | Pin a folder image URL to NVS |

---

## FolderImageSource

Fetches an HTTP directory listing, parses image links, and downloads a specific image by index.

Supports two server response formats:
- **HTML directory listing** (nginx autoindex, Apache, Python `http.server`): scans for `href="..."` values ending in `.jpg`, `.jpeg`, `.png`, `.bmp`
- **JSON array** (explicit control): if response starts with `[`, treats content as `["file1.jpg", ...]`

Images are sorted alphabetically for deterministic ordering across reboots.

---

## Development Conventions

### Adding new config fields

1. Add the field to the `ApplicationConfig` struct in `ApplicationConfig.h`
2. Initialise it in the `ApplicationConfig()` constructor
3. Add NVS read/write in `ApplicationConfigStorage.cpp`
4. Add a template placeholder in `data/config.html`
5. Handle the form field in `ConfigurationServer::handleSave()`
6. Propagate to the `Configuration` struct in `ConfigurationServer.h`
7. Assign from `Configuration` to `ApplicationConfig` in the `onSaveCallback` in `main.cpp`

### Adding new dithering algorithms

Add a new case to `DitherMode` enum in `ApplicationConfig.h`, implement the algorithm as a private method in `ImageScreen.cpp`, and add the case to the `switch` in `ImageScreen::ditherImage()`.

### Local development credentials

Create `src/config_dev.h` (already gitignored) with:

```cpp
#ifndef CONFIG_DEV_H
#define CONFIG_DEV_H
const char DEFAULT_WIFI_SSID[]     = "YourNetwork";
const char DEFAULT_WIFI_PASSWORD[] = "YourPassword";
const char DEFAULT_IMAGE_URL[]     = "https://example.com/image.jpg";
#endif
```

`ApplicationConfig.h` includes this file preferentially via `__has_include`.

### Manufacturer driver files

`GDEP133C02.c`, `comm.c`, `pindefine.h`, and `status.h` are adapted from Good-Display's official ESP-IDF example. Treat these as a sealed HAL layer. Do not modify them for application-level changes.

---

## Parked / Known Issues

- **SD card support:** Pin sharing between SD and display was investigated and code was written, but mounting fails on hardware. Needs physical debugging with a logic analyser. The `copyImageFromSDToLittleFS()` call exists in `main.cpp` but may silently fail on the target hardware.
- **Battery pin:** `BATTERY_PIN` is set to `-1` in `battery.h`, disabling ADC reads. The correct GPIO for this board has not been confirmed.
- **`boards.h`:** The `include/boards.h` file exists but is not `#include`d in firmware to avoid pin conflicts with `GDEP133C02`.

---

## Spectra 6 Colour Palette

| Index | Colour | RGB |
|---|---|---|
| 0 | Black | `(0, 0, 0)` |
| 1 | White | `(255, 255, 255)` |
| 2 | Yellow | `(230, 230, 0)` |
| 3 | Red | `(204, 0, 0)` |
| 4 | Blue | `(0, 51, 204)` |
| 5 | Green | `(0, 204, 0)` |

Colour matching uses Euclidean distance in RGB space: `distance = dr² + dg² + db²`.

---

## Flash Partition Layout

Defined in `partitions.csv`:

| Partition | Offset | Size | Purpose |
|---|---|---|---|
| `nvs` | 0x9000 | 20 KB | WiFi credentials, config, img_index |
| `otadata` | 0xE000 | 8 KB | OTA metadata |
| `app0` (ota_0) | 0x10000 | 2.3 MB | Firmware |
| `spiffs` (LittleFS) | 0x260000 | 5.6 MB | Images + config.html |
