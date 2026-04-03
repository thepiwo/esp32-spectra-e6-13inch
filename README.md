# ESP32-133C02 — 13.3" E-Ink Spectra 6 Display Firmware

Custom firmware for the **Good-Display 13.3-inch E-Ink Spectra 6 panel (GDEP133C02)** paired with the **ESP32-133C02** driver board. Displays full-color (6-colour) images uploaded via a built-in web portal, then enters deep sleep to preserve the image indefinitely.

Originally adapted from [shi-314/esp32-spectra-e6](https://github.com/shi-314/esp32-spectra-e6) (MIT License), which targeted a smaller display. This fork adds full support for the 13.3" panel's dual-IC QSPI interface, image upload via a web server, multiple dithering algorithms, JPEG/PNG/BMP decoding, automatic image scaling, HTTP folder cycling, pre-encoded `.spectra6` format support, quiet hours, and deep sleep management.

---

## Table of Contents

- [Features](#features)
- [Hardware](#hardware)
- [Pin Mapping](#pin-mapping)
- [SD Card Pins](#sd-card-pins--shared-spi-bus)
- [Architecture Overview](#architecture-overview)
- [Boot Flow](#boot-flow)
- [Image Processing Pipeline](#image-processing-pipeline)
- [Pre-Encoded Spectra6 Format](#pre-encoded-spectra6-format)
- [Setup & Deployment](#setup--deployment)
- [Configuration](#configuration)
- [Quiet Hours](#quiet-hours)
- [Flash Partition Layout](#flash-partition-layout)
- [Dependencies & Libraries](#dependencies--libraries)
- [Source Code Structure](#source-code-structure)
- [Memory Management](#memory-management)
- [License](#license)

---

## Features

- **6-colour rendering** — Black, White, Yellow, Red, Blue, Green via the Spectra 6 palette.
- **Web-based image upload** — Upload JPEG, PNG, or BMP images directly through a browser.
- **HTTP image folder cycling** — Point the device at an HTTP directory (nginx autoindex, Python `http.server`, NAS share, etc.) and it cycles through images in alphabetical order on each wake.
- **Pre-encoded `.spectra6` format** — Serve pre-dithered images from a companion converter tool. The device skips all on-device decoding — just a direct memory copy into the display. Dramatically reduces PSRAM usage and boot time.
- **User-selectable dithering** — Choose between Floyd-Steinberg, Atkinson, Ordered (Bayer), or Nearest Neighbour. Dithering controls are hidden in the web portal when a `.spectra6` URL is configured.
- **Automatic image scaling** — Images of any size are nearest-neighbour scaled in-place to fit 1200×1600, with aspect ratio preserved via automatic letterboxing/pillarboxing.
- **Quiet hours** — Configure a daily time window (e.g. 11 pm–8 am) during which the device skips its noisy e-ink refresh and sleeps directly until the window ends. Requires WiFi for NTP time sync.
- **Timed deep sleep** — Wake at configurable intervals (15 min, 30 min, 1 h, etc.) to cycle images. Deep sleep preserves the displayed image with zero power draw.
- **PSRAM-optimised** — All large buffers are allocated in the ESP32-S3's 8 MB PSRAM.
- **Dual-IC QSPI** — Custom `DisplayAdapter` bridges the manufacturer's C driver into an `Adafruit_GFX`-compatible API, handling the split framebuffer across two driver ICs.
- **WiFi configuration portal** — First-boot Access Point mode with a web UI for entering WiFi credentials, image URL, and all display settings.
- **NVS persistent storage** — All configuration survives deep sleep and power cycles.
- **LittleFS image storage** — Uploaded images are stored on the internal flash filesystem (~5.6 MB partition).

---

## Hardware

| Component | Details |
|---|---|
| **Driver Board** | ESP32-133C02 (Good-Display) |
| **MCU** | ESP32-S3-WROOM-1 (N16R8) — 16 MB Flash, 8 MB PSRAM |
| **Display Panel** | GDEP133C02 — 13.3" E-Ink Spectra 6 |
| **Resolution** | 1200 × 1600 pixels |
| **Colours** | 6 (Black, White, Yellow, Red, Blue, Green) |
| **Interface** | QSPI with dual driver ICs (CS0 + CS1) |
| **Power** | USB-C or battery (with brownout detection) |

---

## Pin Mapping

### Display (QSPI)

The display uses a **Quad-SPI** interface with **two chip-select lines** (one per driver IC — each IC handles half the display width).

| Function | GPIO | Direction | Notes |
|---|---|---|---|
| `SPI_CLK` | **9** | Output | SPI clock |
| `SPI_Data0` | **41** | Bidirectional | QSPI data line 0 |
| `SPI_Data1` | **40** | Bidirectional | QSPI data line 1 |
| `SPI_Data2` | **39** | Bidirectional | QSPI data line 2 |
| `SPI_Data3` | **38** | Bidirectional | QSPI data line 3 |
| `SPI_CS0` | **18** | Output | Chip select — left half (driver IC 0) |
| `SPI_CS1` | **17** | Output | Chip select — right half (driver IC 1) |
| `EPD_BUSY` | **7** | Input | Display busy signal |
| `EPD_RST` | **6** | Output | Display hardware reset |
| `LOAD_SW` | **45** | Output | Load switch (power to display) |

### Switches (optional, currently unused in firmware)

| Function | GPIO | Notes |
|---|---|---|
| `SW_2` | **13** | External pull-down on board |
| `SW_4` | **21** | User button |

---

## SD Card Pins & Shared SPI Bus

> **Important:** The SD card slot on the ESP32-133C02 board shares the **same SPI data and clock lines** as the E-Ink display. This is a critical design detail.

### SD Card Pin Mapping

| Function | GPIO | Shared With |
|---|---|---|
| **SD_CLK** (SCK) | **9** | `SPI_CLK` (display) |
| **SD_CMD** (MOSI) | **41** | `SPI_Data0` (display) |
| **SD_D0** (MISO) | **40** | `SPI_Data1` (display) |
| **SD_CS** | **21** | `SW_4` (user button) |

The display and SD card **cannot be used simultaneously**. The firmware accesses the SD card first (before display initialisation), copies any found image to LittleFS, then releases the bus for the display driver.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        main.cpp (setup)                        │
│  Boot → WiFi → NTP → Quiet Hours → Display → Web Server → Sleep│
├─────────────┬───────────────┬───────────────┬──────────────────┤
│ ImageScreen │ ConfigScreen  │ ConfigServer  │ WiFiConnection   │
│  (render)   │   (AP mode)   │ (web portal)  │  (STA connect)   │
├─────────────┴───────────────┴───────────────┴──────────────────┤
│                     DisplayAdapter                              │
│        Adafruit_GFX subclass → PSRAM framebuffer               │
│      Handles rotation, colour packing, dual-IC split           │
├─────────────────────────────────────────────────────────────────┤
│              Manufacturer C Driver Layer                        │
│   GDEP133C02.c  +  comm.c  +  pindefine.h                     │
│   QSPI init, GPIO config, EPD commands, power sequencing       │
├─────────────────────────────────────────────────────────────────┤
│                    ESP32-S3 Hardware                            │
│           SPI3_HOST  •  8MB PSRAM  •  16MB Flash               │
└─────────────────────────────────────────────────────────────────┘
```

---

## Boot Flow

```
Power On / Timer Wake
      │
      ▼
  Load config from NVS
      │
      ├── Timer wake + quiet hours configured?
      │       │
      │       ├── Connect WiFi → sync NTP
      │       │       ├── In quiet window → sleep until end of window
      │       │       └── Outside window → proceed
      │       └── NTP unavailable → sleep and retry
      │
      ▼
  SD Card: copy image to LittleFS (if present)
      │
      ▼
  Connect to WiFi
      │
      ├── Has credentials?
      │       │
      │       ├── YES → Connect
      │       │         ▼
      │       │    Advance image index (if timer wake + folder configured)
      │       │         ▼
      │       │    Display image (LittleFS → Folder → Single URL, in priority order)
      │       │         ▼
      │       │    Web server runs for 10 minutes  [skipped on timer wake]
      │       │         ▼
      │       │    Enter deep sleep (timer or permanent)
      │       │
      │       └── NO → Display stored image
      │                Start Access Point ("Framey-Config")
      │                Run config portal for 10 minutes
      │                Enter deep sleep
```

### Image Priority

When multiple image sources are configured, the device uses this priority order:

1. **Pinned folder image** — a specific folder image pinned via the web portal
2. **Local LittleFS image** — uploaded via the web portal
3. **HTTP folder** — cycles through images in alphabetical order
4. **Single image URL** — downloads a single image each wake

---

## Image Processing Pipeline

### Standard path (JPEG / PNG / BMP)

```
Raw Image File (JPEG / PNG / BMP)
      │
      ▼
  Format Detection (magic bytes: FFD8=JPEG, 89PNG=PNG, BM=BMP)
      │
      ├── JPEG: JPEGDEC → RGB565 buffer (PSRAM)
      ├── PNG:  PNGdec  → RGB565 buffer (streaming or in-memory)
      └── BMP:  Manual 24-bit parser → RGB565 buffer
      │
      ▼
  Scale-to-Fit (nearest-neighbour, aspect-ratio preserved)
  White letterbox/pillarbox bars on mismatched aspect ratios
      │
      ▼
  Dithering (user-selectable):
  ├── Floyd-Steinberg  (smooth gradients, default)
  ├── Atkinson         (higher contrast)
  ├── Ordered/Bayer    (structured pattern)
  └── None             (nearest colour, for vector art)
      │
      ▼
  5 colour bitmaps (1-bit each):
  Black, Yellow, Red, Blue, Green  (White = no bits set)
      │
      ▼
  Render via DisplayAdapter::drawBitmap() → 4-bit PSRAM framebuffer
      │
      ▼
  Send to display via QSPI (dual-IC split) → ~20 second refresh
```

### Pre-encoded path (`.spectra6`)

```
Pre-encoded .spectra6 file (served over HTTP)
      │
      ▼
  Magic header detection ("SPECTRA6")
      │
      ▼
  Direct memcpy into 5 colour bitmaps — no decode, no dithering
      │
      ▼
  Render via DisplayAdapter::drawBitmap() → 4-bit PSRAM framebuffer
      │
      ▼
  Send to display via QSPI → ~20 second refresh
```

### Supported Image Formats

| Format | Notes |
|---|---|
| **JPEG** | Any size; pre-scaled by JPEGDEC before full decode when very large |
| **PNG** | Any size; supports alpha channel |
| **BMP** | 24-bit uncompressed |
| **`.spectra6`** | Pre-encoded binary format — zero on-device dithering or decoding |

### Display Dimensions

| Property | Value |
|---|---|
| **Physical screen** | 13.3 inches diagonal |
| **Native resolution** | 1200 × 1600 pixels |
| **Orientation** | Portrait |
| **Aspect ratio** | 3:4 |
| **Colour depth** | 6 colours |
| **Refresh time** | ~20 seconds |

The ideal source image is **1200 × 1600 px portrait**. Any other size is scaled automatically.

---

## Pre-Encoded Spectra6 Format

The `.spectra6` format lets you do all image processing on a PC and serve the result directly to the device — the device just copies the data into the display pipeline with no CPU or memory overhead.

### Why use it?

| | Standard (JPEG/PNG) | Pre-encoded (`.spectra6`) |
|---|---|---|
| **On-device dithering** | Yes (~seconds) | None |
| **Peak PSRAM** | ~5 MB | ~2.4 MB |
| **Download size** | 1–8 MB | ~1.17 MB (fixed) |
| **Dithering quality** | Good | Best (full floating-point on PC) |

### File format

```
Bytes 0–7:   ASCII magic "SPECTRA6" (no null terminator)
Bytes 8–11:  uint32_t width  (little-endian)
Bytes 12–15: uint32_t height (little-endian)
Bytes 16+:   5 × planeSize bytes  (black, yellow, red, blue, green)
             planeSize = ((width + 7) / 8) × height
```

Total size for 1200×1600: **1,200,016 bytes (~1.17 MB)**

Each plane is a 1-bpp MSB-first row-major bitmap. White pixels have no bit set in any plane.

### Converter tool

Use the companion Python converter to produce `.spectra6` files from standard images:

👉 **[PhotoPainter-E-Ink-Spectra-6-image-converter](https://github.com/thepiwo/PhotoPainter-E-Ink-Spectra-6-image-converter)** — companion converter that produces `.spectra6` files from standard images

```bash
python ConvertTo6ColorsForEInkSpectra6.py image.jpg --format spectra6 --dither 3
```

- `--dither 3` — Floyd-Steinberg (recommended; produces fuller, more saturated output than the Atkinson default)
- `--dither 1` — Atkinson
- Target resolution: **1200×1600** (portrait)

The device auto-detects `.spectra6` files by their magic header — no configuration needed. When a `.spectra6` URL is entered in the web portal, the dithering selector is automatically hidden.

---

## Setup & Deployment

### Prerequisites

- [PlatformIO](https://platformio.org/) installed (e.g., VSCode extension)
- ESP32-133C02 board connected via USB-C

### Build & Flash

```bash
# Clone the repository
git clone https://github.com/dandwhelan/esp32-spectra-e6-13inch.git
cd esp32-spectra-e6-13inch

# Build and upload firmware
pio run --target upload

# Upload the LittleFS filesystem (web portal HTML)
pio run --target uploadfs

# Monitor serial output
pio device monitor -b 115200
```

### Optional: Custom WiFi Defaults

Create `src/config_dev.h` (gitignored) to set default WiFi credentials for development:

```cpp
#ifndef CONFIG_DEV_H
#define CONFIG_DEV_H

const char DEFAULT_WIFI_SSID[]     = "YourNetwork";
const char DEFAULT_WIFI_PASSWORD[] = "YourPassword";
const char DEFAULT_IMAGE_URL[]     = "https://example.com/image.png";

#endif
```

> **Note:** Adding new fields to `ApplicationConfig` changes the NVS blob size. On first boot after a firmware update that adds fields, stored settings are automatically cleared and the device uses defaults. Re-enter your settings via the web portal.

---

## Configuration

### First Boot (No Credentials)

1. The device creates a WiFi Access Point: **`Framey-Config`** (password: `configure123`)
2. Connect with your phone or laptop
3. Navigate to `http://192.168.4.1`
4. Enter your WiFi SSID, password, and image URL
5. Save — settings are stored in NVS and survive reboots and deep sleep

### Web Portal Settings

Once configured, the device's web portal is available at `http://<device-ip>` during the 10-minute server window after each boot.

| Setting | Description |
|---|---|
| **Single Image URL** | Direct URL to a JPEG, PNG, BMP, or `.spectra6` file |
| **Image Folder URL** | HTTP directory URL; device cycles images alphabetically |
| **Dithering Algorithm** | Floyd-Steinberg / Atkinson / Ordered / None (hidden for `.spectra6` URLs) |
| **Scaling Mode** | Fill (crop to cover) or Fit (letterbox) |
| **Change Image Every** | How often to advance to the next folder image |
| **Wake From Sleep Every** | How often the device wakes from deep sleep |
| **UTC Offset (hours)** | Your timezone offset from UTC, used for quiet hours |
| **Quiet From / Until** | Hour range (0–23) during which the display will not refresh |

### Folder Cycling

Set **Image Folder URL** to an HTTP directory that serves image files. The device:

1. Fetches the directory listing (supports nginx autoindex, Apache, Python `http.server`, JSON arrays)
2. Sorts filenames alphabetically for deterministic ordering
3. Advances to the next image on each wake (subject to **Change Image Every** interval)

You can also browse the folder from the web portal and **pin** a specific image to display it indefinitely.

---

## Quiet Hours

Quiet hours prevent the noisy e-ink refresh during set times — useful for bedrooms.

- Set **UTC Offset**, **Quiet From** (start hour, 0–23), and **Quiet Until** (end hour, 0–23)
- Midnight-spanning ranges work: e.g. start=23, end=8 means 11 pm to 8 am
- Set both to the same value to disable
- Requires WiFi — if the device cannot reach NTP, it sleeps for the normal sleep interval and retries on the next wake
- On a quiet-hours wake, the device sleeps precisely until the end of the quiet window (e.g. if it wakes at 3 am with quiet hours until 8 am, it sleeps exactly 5 hours)

---

## Flash Partition Layout

| Partition | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| `nvs` | data (nvs) | 0x9000 | 20 KB | WiFi credentials, config, image index |
| `otadata` | data (ota) | 0xE000 | 8 KB | OTA metadata |
| `app0` | app (ota_0) | 0x10000 | 2.3 MB | Firmware |
| `spiffs` | data (spiffs) | 0x260000 | **5.6 MB** | LittleFS (uploaded images + web portal HTML) |

---

## Dependencies & Libraries

All dependencies are managed by PlatformIO and declared in `platformio.ini`.

### Hardware/Display

| Library | Purpose |
|---|---|
| **Adafruit GFX** | Base graphics primitives (inherited by DisplayAdapter) |
| **U8g2** | Font rendering engine |
| **U8g2_for_Adafruit_GFX** | Bridge between U8g2 fonts and Adafruit_GFX |

### Image Decoding

| Library | Purpose |
|---|---|
| **JPEGDEC** | JPEG decoding with hardware scaling |
| **PNGdec** | PNG decoding (streaming from file or memory) |

### Networking

| Library | Purpose |
|---|---|
| **ESPAsyncWebServer** | Async HTTP server for config portal and image uploads |
| **AsyncTCP** | Async TCP layer (required by ESPAsyncWebServer) |
| **WiFi** | WiFi STA and AP mode |
| **HTTPClient** | Image download from URL |
| **DNSServer** | Captive portal in AP mode |

### Utility

| Library | Purpose |
|---|---|
| **qrcode** | QR code generation for config screen |
| **FS / LittleFS** | Filesystem for image and HTML storage |

### Upstream / Example Code

| Repository | Relationship |
|---|---|
| [shi-314/esp32-spectra-e6](https://github.com/shi-314/esp32-spectra-e6) | **Original project** — firmware for smaller Spectra 6 displays. This repo forked the image processing, dithering, WiFi setup, and config portal logic. |
| [Good-Display example code](https://www.good-display.com/) | **Manufacturer C driver** — `GDEP133C02.c`, `comm.c`, `pindefine.h` are adapted from Good-Display's official ESP-IDF example. These handle QSPI initialisation, EPD commands, and dual-IC communication. |

---

## Source Code Structure

```
src/
├── main.cpp                    # Boot flow, WiFi, NTP, quiet hours gate, web server, deep sleep
│
├── DisplayAdapter.cpp/.h       # Adafruit_GFX subclass wrapping the QSPI driver
│                                 PSRAM framebuffer, dual-IC split transfer
│
├── ImageScreen.cpp/.h          # Image loading pipeline:
│                                 JPEG/PNG/BMP/Spectra6 decode, scaling, dithering,
│                                 bitmap rendering; LittleFS + HTTP folder + URL sources
│
├── FolderImageSource.cpp/.h    # HTTP directory listing parser + image downloader
│                                 Supports HTML autoindex and JSON array formats
│
├── ConfigurationServer.cpp/.h  # Async web server: config portal, image upload, folder browse
├── ConfigurationScreen.cpp/.h  # AP mode display (QR code + connection info)
│
├── WiFiConnection.cpp/.h       # WiFi STA connection manager
├── HttpDownloader.cpp/.h       # HTTP/HTTPS image downloader with ETag caching
├── SDCardManager.cpp/.h        # SD card → LittleFS image copy (runs before display init)
│
├── ApplicationConfig.h         # Runtime config struct (all settings)
├── ApplicationConfigStorage    # NVS read/write for persistent config + image index
├── config_default.h            # Default empty credentials (safe to commit)
│
├── GDEP133C02.c/.h             # [Manufacturer] EPD init, command sequences, refresh
├── comm.c/.h                   # [Manufacturer] SPI bus init, GPIO, transactions
├── pindefine.h                 # [Manufacturer] GPIO pin assignments
├── status.h                    # [Manufacturer] Debug flag
│
├── battery.cpp/.h              # Battery voltage ADC (currently disabled)
└── Screen.h                    # Abstract screen interface

data/
└── config.html                 # Web portal HTML (uploaded to LittleFS via uploadfs)

platformio.ini                  # Build config, library dependencies, partition table
partitions.csv                  # Custom flash partition layout
```

---

## Memory Management

The ESP32-S3's **8 MB PSRAM** is the critical resource.

### Standard image path (JPEG/PNG/BMP)

| Buffer | Size | Notes |
|---|---|---|
| Display framebuffer | 960 KB | 4-bit packed, 2 pixels/byte, always allocated |
| RGB565 decode buffer | 3.84 MB | Allocated during decode, freed before dithering |
| Colour bitmaps (×5) | 1.2 MB | 1-bit per pixel per colour |
| **Peak concurrent** | **~6 MB** | RGB565 + bitmaps + framebuffer |

For large JPEGs the raw source data is freed immediately after decode — before dithering begins — to avoid exceeding 8 MB.

### Pre-encoded `.spectra6` path

| Buffer | Size | Notes |
|---|---|---|
| Display framebuffer | 960 KB | Always allocated |
| Download buffer | ~1.14 MB | The `.spectra6` file itself |
| Colour bitmaps (×5) | 1.2 MB | Direct memcpy from download buffer |
| **Peak concurrent** | **~3.3 MB** | ~45% less than standard path |

---

## License

MIT License. Original base logic by [shi-314](https://github.com/shi-314/esp32-spectra-e6). Adapted and extended by [dandwhelan](https://github.com/dandwhelan).
