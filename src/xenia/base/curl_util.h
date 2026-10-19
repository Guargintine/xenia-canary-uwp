/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_CURL_UTIL_H_
#define XENIA_BASE_CURL_UTIL_H_

// clang-format off
#include "xenia/base/platform.h"
#include "third_party/libcurl/include/curl/curl.h"
// clang-format on

namespace xe {

// Creates an easy handle with the host's HTTPS trust configuration. The caller
// owns the returned handle and must release it with curl_easy_cleanup.
CURL* CreateCurlHandle();

}  // namespace xe

#endif  // XENIA_BASE_CURL_UTIL_H_
