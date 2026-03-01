#include "SDCardManager.h"

#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

extern "C" {
#include "pindefine.h"
}

// ---------------------------------------------------------------------------
// SPI instance
// The SD card has its own data pins (GPIO 3, 4) separate from the display,
// but shares GPIO 18 (display CS0 = SD SCK). We use FSPI (SPI2_HOST)
// to keep it on a different SPI peripheral from the display's SPI3_HOST.
// SD card access must complete BEFORE display init claims GPIO 18 as CS0.
// ---------------------------------------------------------------------------
static SPIClass sdSPI(FSPI);

// LittleFS destinations — must match what ImageScreen::loadFromLittleFS() looks
// for.
static const char *LFS_IMAGE_NAMES[] = {"/local_image.bmp", "/local_image.jpg",
                                        "/local_image.jpeg",
                                        "/local_image.png"};

// Preferred filenames tried first (in order) before falling back to a
// directory scan.  This gives predictable behaviour when more than one image
// is on the card.
static const char *SD_PREFERRED_NAMES[] = {
    "/image.jpg", "/image.jpeg", "/image.png", "/image.bmp",
    "/IMAGE.JPG", "/IMAGE.JPEG", "/IMAGE.PNG", "/IMAGE.BMP",
};
static const size_t SD_PREFERRED_COUNT =
    sizeof(SD_PREFERRED_NAMES) / sizeof(SD_PREFERRED_NAMES[0]);

static bool isImageExtension(const char *name) {
  String s(name);
  s.toLowerCase();
  return s.endsWith(".jpg") || s.endsWith(".jpeg") || s.endsWith(".png") ||
         s.endsWith(".bmp");
}

// Scan the SD root and return the first image file found (any name).
// Returns an empty String if none found.
static String findAnyImageOnSD() {
  File root = SD.open("/");
  if (!root)
    return "";
  while (true) {
    File entry = root.openNextFile();
    if (!entry)
      break;
    if (!entry.isDirectory() && isImageExtension(entry.name())) {
      String found = "/";
      found += entry.name();
      entry.close();
      root.close();
      return found;
    }
    entry.close();
  }
  root.close();
  return "";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *cardTypeName(uint8_t t) {
  switch (t) {
  case CARD_MMC:
    return "MMC";
  case CARD_SD:
    return "SDSC";
  case CARD_SDHC:
    return "SDHC/SDXC";
  case CARD_NONE:
    return "none";
  default:
    return "unknown";
  }
}

static const char *lfsDestForExtension(const char *sdPath) {
  String path(sdPath);
  path.toLowerCase();
  if (path.endsWith(".bmp"))
    return "/local_image.bmp";
  if (path.endsWith(".jpg"))
    return "/local_image.jpg";
  if (path.endsWith(".jpeg"))
    return "/local_image.jpeg";
  if (path.endsWith(".png"))
    return "/local_image.png";
  return "/local_image.bin";
}

static void removeOldLocalImages() {
  for (const char *name : LFS_IMAGE_NAMES) {
    if (LittleFS.exists(name)) {
      printf("SD: removing stale LittleFS file: %s\r\n", name);
      LittleFS.remove(name);
    }
  }
}

// Print every file in the SD card root so the user can see exactly what's
// there.
static void listSDRoot() {
  printf("SD: --- root directory listing ---\r\n");
  File root = SD.open("/");
  if (!root) {
    printf("SD:   (failed to open root)\r\n");
    return;
  }
  int count = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry)
      break;
    if (!entry.isDirectory()) {
      printf("SD:   %-30s  %u bytes\r\n", entry.name(), (unsigned)entry.size());
      count++;
    }
    entry.close();
  }
  root.close();
  if (count == 0) {
    printf("SD:   (no files found in root)\r\n");
  }
  printf("SD: --- end of listing (%d file(s)) ---\r\n", count);
}

// Print LittleFS usage so we know if there is room before we try to copy.
static bool checkLittleFSSpace(size_t needed) {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  size_t free = total - used;
  printf("SD: LittleFS  total=%u KB  used=%u KB  free=%u KB\r\n",
         (unsigned)(total / 1024), (unsigned)(used / 1024),
         (unsigned)(free / 1024));
  if (free < needed + 4096 /* leave 4 KB headroom */) {
    printf("SD: ERROR — not enough LittleFS space (need %u KB, have %u KB "
           "free)\r\n",
           (unsigned)(needed / 1024), (unsigned)(free / 1024));
    return false;
  }
  return true;
}

static bool copyFile(fs::FS &srcFS, const char *srcPath, fs::FS &dstFS,
                     const char *dstPath) {
  File src = srcFS.open(srcPath, FILE_READ);
  if (!src) {
    printf("SD: ERROR — failed to open %s for reading\r\n", srcPath);
    return false;
  }

  size_t fileSize = src.size();
  printf("SD: source file: %s  (%u bytes / %.1f KB)\r\n", srcPath,
         (unsigned)fileSize, fileSize / 1024.0f);

  File dst = dstFS.open(dstPath, FILE_WRITE);
  if (!dst) {
    printf("SD: ERROR — failed to open LittleFS %s for writing\r\n", dstPath);
    src.close();
    return false;
  }

  printf("SD: copying -> LittleFS %s\r\n", dstPath);

  // Copy in 4 KB chunks; print progress every 10 %
  uint8_t buf[4096];
  size_t totalWritten = 0;
  int lastPct = -1;
  uint32_t t0 = millis();

  while (totalWritten < fileSize) {
    size_t toRead = min((size_t)sizeof(buf), fileSize - totalWritten);
    size_t bytesRead = src.read(buf, toRead);
    if (bytesRead == 0) {
      printf("SD: WARNING — read returned 0 at offset %u\r\n",
             (unsigned)totalWritten);
      break;
    }
    size_t bytesWritten = dst.write(buf, bytesRead);
    if (bytesWritten != bytesRead) {
      printf("SD: ERROR — write failed at offset %u (wrote %u of %u)\r\n",
             (unsigned)totalWritten, (unsigned)bytesWritten,
             (unsigned)bytesRead);
      src.close();
      dst.close();
      return false;
    }
    totalWritten += bytesWritten;

    int pct = (fileSize > 0) ? (int)((totalWritten * 100) / fileSize) : 100;
    if (pct / 10 != lastPct / 10) { // print every ~10 %
      lastPct = pct;
      printf("SD:   %3d%%  (%u / %u bytes)\r\n", pct, (unsigned)totalWritten,
             (unsigned)fileSize);
    }
  }

  src.close();
  dst.close();

  uint32_t elapsed = millis() - t0;
  float kbps =
      (elapsed > 0) ? (totalWritten / 1024.0f) / (elapsed / 1000.0f) : 0;
  printf("SD: copy done  %u bytes in %u ms  (%.1f KB/s)\r\n",
         (unsigned)totalWritten, (unsigned)elapsed, kbps);

  // Verify the destination file size matches.
  File verify = dstFS.open(dstPath, FILE_READ);
  if (!verify) {
    printf("SD: WARNING — could not reopen LittleFS file to verify size\r\n");
    return totalWritten == fileSize;
  }
  size_t verifySize = verify.size();
  verify.close();
  if (verifySize != fileSize) {
    printf("SD: ERROR — size mismatch after copy! LittleFS=%u, expected=%u\r\n",
           (unsigned)verifySize, (unsigned)fileSize);
    return false;
  }
  printf("SD: verification OK — LittleFS file size matches source\r\n");
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool copyImageFromSDToLittleFS() {
  printf("\r\n");
  printf(
      "SD: ============================================================\r\n");
  printf("SD:  SD card image loader\r\n");
  printf(
      "SD: ============================================================\r\n");

  // --- Phase 1: show wiring -------------------------------------------------
  printf("SD: SPI bus  : FSPI (SPI2_HOST) — separate from display\r\n");
  printf("SD: SCK      : GPIO %d  (SD_SCK, shared with display CS0!)\r\n",
         SD_SCK);
  printf("SD: MOSI     : GPIO %d  (SD_MOSI)\r\n", SD_MOSI);
  printf("SD: MISO     : GPIO %d  (SD_MISO)\r\n", SD_MISO);
  printf("SD: CS (SD)  : GPIO %d  (SD_CS)\r\n", SD_CS);
  printf("SD: CS (EPD0): GPIO %d  (SPI_CS0) — driven HIGH\r\n", SPI_CS0);
  printf("SD: CS (EPD1): GPIO %d  (SPI_CS1) — driven HIGH\r\n", SPI_CS1);

  // --- Phase 2: deselect display --------------------------------------------
  printf("SD: Deselecting display CS pins...\r\n");
  pinMode(SPI_CS0, OUTPUT);
  digitalWrite(SPI_CS0, HIGH);
  pinMode(SPI_CS1, OUTPUT);
  digitalWrite(SPI_CS1, HIGH);
  printf("SD: Display CS0=%d  CS1=%d\r\n", digitalRead(SPI_CS0),
         digitalRead(SPI_CS1));

  // --- Phase 3: mount SD card -----------------------------------------------
  printf("SD: Starting SPI bus at 4 MHz and mounting SD card...\r\n");
  uint32_t t0 = millis();
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS); // SCK=18, MISO=4, MOSI=3, SS=15

  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    printf("SD: ERROR — SD.begin() failed\r\n");
    printf("SD:   Possible causes:\r\n");
    printf("SD:     - No SD card inserted\r\n");
    printf("SD:     - Bad contact / card not seated\r\n");
    printf("SD:     - Card needs 3.3 V (not 5 V)\r\n");
    printf("SD:     - SPI wiring issue on GPIO %d/%d/%d\r\n", SD_SCK, SD_MOSI,
           SD_MISO);
    printf("SD:     - CS pin GPIO %d shorted or floating\r\n", SD_CS);
    SD.end();
    sdSPI.end();
    return false;
  }
  printf("SD: SD.begin() OK (%u ms)\r\n", (unsigned)(millis() - t0));

  // --- Phase 4: card info ---------------------------------------------------
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    printf("SD: ERROR — card reports CARD_NONE after successful begin\r\n");
    SD.end();
    sdSPI.end();
    return false;
  }

  uint64_t cardBytes = SD.cardSize();
  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  printf("SD: Card type : %s\r\n", cardTypeName(cardType));
  printf("SD: Card size : %llu MB  (%llu bytes)\r\n", cardBytes / (1024 * 1024),
         cardBytes);
  printf("SD: FS total  : %llu MB\r\n", totalBytes / (1024 * 1024));
  printf("SD: FS used   : %llu MB\r\n", usedBytes / (1024 * 1024));
  printf("SD: FS free   : %llu MB\r\n",
         (totalBytes - usedBytes) / (1024 * 1024));

  // --- Phase 5: list root directory -----------------------------------------
  listSDRoot();

  // --- Phase 6: find image file ---------------------------------------------
  // Stage 1: try preferred names (gives predictable priority with multiple
  // images). Stage 2: fall back to any image in the root (any filename is
  // accepted).
  printf("SD: Searching for image...\r\n");

  String foundPathStr;

  for (size_t i = 0; i < SD_PREFERRED_COUNT; i++) {
    if (SD.exists(SD_PREFERRED_NAMES[i])) {
      foundPathStr = SD_PREFERRED_NAMES[i];
      printf("SD: Found preferred file: %s\r\n", foundPathStr.c_str());
      break;
    }
  }

  if (foundPathStr.isEmpty()) {
    printf(
        "SD: No preferred name matched — scanning root for any image...\r\n");
    foundPathStr = findAnyImageOnSD();
    if (!foundPathStr.isEmpty()) {
      printf("SD: Found by scan: %s\r\n", foundPathStr.c_str());
    }
  }

  if (foundPathStr.isEmpty()) {
    printf("SD: ERROR — no image file found on card\r\n");
    printf("SD:   Place a .jpg / .png / .bmp file in the root of the SD "
           "card.\r\n");
    printf("SD:   Any filename is accepted; 'image.jpg' is tried first.\r\n");
    SD.end();
    sdSPI.end();
    return false;
  }

  const char *foundPath = foundPathStr.c_str();

  // --- Phase 7: prepare LittleFS --------------------------------------------
  printf("SD: Mounting LittleFS...\r\n");
  if (!LittleFS.begin(true)) {
    printf("SD: ERROR — LittleFS.begin() failed\r\n");
    SD.end();
    sdSPI.end();
    return false;
  }

  size_t srcSize = 0;
  {
    File tmp = SD.open(foundPath, FILE_READ);
    if (tmp) {
      srcSize = tmp.size();
      tmp.close();
    }
  }

  if (!checkLittleFSSpace(srcSize)) {
    LittleFS.end();
    SD.end();
    sdSPI.end();
    return false;
  }

  removeOldLocalImages();

  // --- Phase 8: copy --------------------------------------------------------
  const char *dstPath = lfsDestForExtension(foundPath);
  bool ok = copyFile(SD, foundPath, LittleFS, dstPath);

  // --- Phase 9: LittleFS summary --------------------------------------------
  if (ok) {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    printf("SD: LittleFS after copy — used=%u KB / total=%u KB\r\n",
           (unsigned)(used / 1024), (unsigned)(total / 1024));
  }

  LittleFS.end();

  // --- Phase 10: release SPI bus --------------------------------------------
  printf("SD: Releasing SPI bus (display will re-claim it next)...\r\n");
  SD.end();
  sdSPI.end();

  printf(
      "SD: ============================================================\r\n");
  if (ok) {
    printf("SD:  SUCCESS — display will load image from LittleFS\r\n");
  } else {
    printf("SD:  FAILED  — display will fall back to HTTP download\r\n");
  }
  printf(
      "SD: "
      "============================================================\r\n\r\n");

  return ok;
}
