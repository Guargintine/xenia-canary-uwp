/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/curl_util.h"

namespace xe {

#if XE_PLATFORM_WINRT
namespace {

// Embed CA roots for Xbox hosts without accessible certificate stores.
constexpr unsigned char kCaBundle[] = {
#include "xenia_ca_bundle.inc"
};

}  // namespace
#endif

CURL* CreateCurlHandle() {
  CURL* handle = curl_easy_init();
#if XE_PLATFORM_WINRT
  if (handle) {
    // The embedded storage lives for the process lifetime; libcurl copies the
    // descriptor and may retain this data without allocating another bundle.
    curl_blob ca_bundle = {const_cast<unsigned char*>(kCaBundle),
                           sizeof(kCaBundle), CURL_BLOB_NOCOPY};
    if (curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &ca_bundle) != CURLE_OK ||
        curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA) !=
            CURLE_OK) {
      curl_easy_cleanup(handle);
      return nullptr;
    }
    // Retain libcurl's default peer and hostname verification. Native roots,
    // when available, remain usable alongside the embedded public roots.
  }
#endif
  return handle;
}

}  // namespace xe
