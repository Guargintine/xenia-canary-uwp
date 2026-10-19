// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/app/screenscraper_service.h"

// Generated in the build tree, never checked into the source repository.
#if __has_include("xenia/screenscraper_build_config.h")
#include "xenia/screenscraper_build_config.h"
#endif
#ifndef XENIA_SCREENSCRAPER_DEV_ID
#define XENIA_SCREENSCRAPER_DEV_ID ""
#endif
#ifndef XENIA_SCREENSCRAPER_DEV_PASSWORD
#define XENIA_SCREENSCRAPER_DEV_PASSWORD ""
#endif

namespace xe::app::screenscraper {
DeveloperIdentity GetDeveloperIdentity() {
  return {XENIA_SCREENSCRAPER_DEV_ID, XENIA_SCREENSCRAPER_DEV_PASSWORD,
          "Xenia Canary"};
}
}  // namespace xe::app::screenscraper
