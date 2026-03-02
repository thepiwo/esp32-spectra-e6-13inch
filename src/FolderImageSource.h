#pragma once

#include <Arduino.h>
#include <memory>
#include <vector>

#include "HttpDownloader.h"

class FolderImageSource {
public:
  // Fetches the directory listing from folderUrl, parses image links,
  // sorts them alphabetically, and downloads the image at the given index.
  // totalImages is set to the number of images found in the directory.
  // Returns nullptr on failure.
  std::unique_ptr<DownloadResult> fetchImage(const String &folderUrl,
                                              uint16_t imageIndex,
                                              uint16_t &totalImages);

  // Download a specific image by its full URL (used for pinned images)
  std::unique_ptr<DownloadResult> fetchImageByUrl(const String &imageUrl);

  // Fetch an HTML directory listing page (accepts any content type)
  String fetchDirectoryListing(const String &url);

  // Parse image file links from an HTML directory listing
  std::vector<String> parseImageLinks(const String &html,
                                       const String &baseUrl);

private:
  HttpDownloader downloader;
};
