#pragma once
#include <Arduino.h>
#include <functional>

#ifndef OTA_REPO
#define OTA_REPO "53Aries/TLTB_OTA"
#endif

namespace Ota {
  struct Callbacks {
    // Optional callbacks for UI integration
    std::function<void(const char*)> onStatus;    // status text updates
    std::function<void(size_t,size_t)> onProgress; // bytes written, total (or 0 if unknown)
  };

  // Performs OTA from a GitHub latest release.
  // repo should be in the form "owner/repo" (e.g., "53Aries/TLTB_OTA").
  // If repo is nullptr, uses OTA_REPO.
  // Returns true on success (device will reboot after success), false on failure.
  bool updateFromGithubLatest(const char* repo, const Callbacks& cb = {});
}
