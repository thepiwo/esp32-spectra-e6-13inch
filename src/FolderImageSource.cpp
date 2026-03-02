#include "FolderImageSource.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>

String FolderImageSource::fetchDirectoryListing(const String &url) {
  HTTPClient http;

  if (url.startsWith("https://")) {
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setInsecure();
    http.begin(*client, url);
  } else {
    WiFiClient *client = new WiFiClient;
    http.begin(*client, url);
  }

  http.setTimeout(10000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Folder: HTTP %d fetching directory listing\n", httpCode);
    http.end();
    return "";
  }

  String body = http.getString();
  http.end();
  return body;
}

static bool isImageExtension(const String &filename) {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg") ||
         lower.endsWith(".png") || lower.endsWith(".bmp");
}

std::vector<String> FolderImageSource::parseImageLinks(const String &html,
                                                       const String &baseUrl) {
  std::vector<String> images;

  // Ensure baseUrl ends with /
  String base = baseUrl;
  if (!base.endsWith("/"))
    base += "/";

  int pos = 0;
  while (pos < (int)html.length()) {
    // Find next href="..."
    int hrefPos = html.indexOf("href=\"", pos);
    if (hrefPos == -1)
      break;
    hrefPos += 6; // skip past href="

    int endQuote = html.indexOf("\"", hrefPos);
    if (endQuote == -1)
      break;

    String link = html.substring(hrefPos, endQuote);
    pos = endQuote + 1;

    // Skip parent/directory links and query parameters
    if (link.startsWith("?") || link.startsWith("..") || link.endsWith("/"))
      continue;

    if (!isImageExtension(link))
      continue;

    // Build full URL
    if (link.startsWith("http://") || link.startsWith("https://")) {
      images.push_back(link);
    } else if (link.startsWith("/")) {
      // Absolute path — extract scheme+host from base
      int schemeEnd = base.indexOf("://");
      if (schemeEnd != -1) {
        int hostEnd = base.indexOf("/", schemeEnd + 3);
        if (hostEnd != -1) {
          images.push_back(base.substring(0, hostEnd) + link);
        } else {
          // No trailing slash on host
          images.push_back(base.substring(0) + link.substring(1));
        }
      }
    } else {
      // Relative filename — append to base
      images.push_back(base + link);
    }
  }

  // Sort alphabetically for deterministic, ordered playback
  std::sort(images.begin(), images.end());

  Serial.printf("Folder: found %d images in directory listing\n",
                images.size());
  for (size_t i = 0; i < images.size() && i < 10; i++) {
    Serial.printf("  [%d] %s\n", i, images[i].c_str());
  }
  if (images.size() > 10) {
    Serial.printf("  ... and %d more\n", images.size() - 10);
  }

  return images;
}

std::unique_ptr<DownloadResult>
FolderImageSource::fetchImage(const String &folderUrl, uint16_t imageIndex,
                              uint16_t &totalImages) {
  Serial.printf("Folder: fetching directory listing from %s\n",
                folderUrl.c_str());

  String html = fetchDirectoryListing(folderUrl);
  if (html.length() == 0) {
    Serial.println("Folder: empty or failed directory listing");
    totalImages = 0;
    return nullptr;
  }

  auto imageUrls = parseImageLinks(html, folderUrl);
  totalImages = imageUrls.size();

  if (totalImages == 0) {
    Serial.println("Folder: no image files found in directory");
    return nullptr;
  }

  // Wrap index around the total number of images
  uint16_t idx = imageIndex % totalImages;

  Serial.printf("Folder: downloading image %d/%d: %s\n", idx + 1, totalImages,
                imageUrls[idx].c_str());

  return downloader.download(imageUrls[idx]);
}

std::unique_ptr<DownloadResult>
FolderImageSource::fetchImageByUrl(const String &imageUrl) {
  Serial.printf("Folder: downloading pinned image: %s\n", imageUrl.c_str());
  return downloader.download(imageUrl);
}
