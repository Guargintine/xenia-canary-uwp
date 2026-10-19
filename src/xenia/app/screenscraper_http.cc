// Copyright 2026 Xenia Canary. Released under the BSD license.
#include "xenia/app/screenscraper_service.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <limits>

#include "xenia/base/curl_util.h"
#include "xenia/base/logging.h"

namespace xe::app::screenscraper {
namespace {
struct Transfer {
  HttpResponse response;
  size_t limit;
  const std::atomic<bool>& cancel;
  bool limit_exceeded = false;
};
size_t Receive(char* bytes, size_t size, size_t count, void* context) {
  auto& transfer = *static_cast<Transfer*>(context);
  if (size && count > std::numeric_limits<size_t>::max() / size) {
    transfer.limit_exceeded = true;
    return 0;
  }
  const auto length = size * count;
  if (transfer.cancel ||
      length > transfer.limit - transfer.response.body.size()) {
    transfer.limit_exceeded = !transfer.cancel;
    return 0;
  }
  try {
    transfer.response.body.insert(transfer.response.body.end(), bytes,
                                  bytes + length);
  } catch (...) {
    return 0;
  }
  return length;
}
size_t Header(char* bytes, size_t size, size_t count, void* context) {
  if (size && count > std::numeric_limits<size_t>::max() / size) {
    return 0;
  }
  const auto length = size * count;
  auto line = std::string_view(bytes, length);
  constexpr std::string_view name = "Retry-After:";
  if (line.size() >= name.size() &&
      std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      })) {
    line.remove_prefix(name.size());
    while (!line.empty() && line.front() == ' ') {
      line.remove_prefix(1);
    }
    uint32_t seconds = 0;
    if (std::from_chars(line.data(), line.data() + line.size(), seconds).ec ==
        std::errc{}) {
      static_cast<Transfer*>(context)->response.retry_after_seconds =
          std::min(seconds, 60u);
    }
  }
  return length;
}
int TransferProgress(void* context, curl_off_t, curl_off_t, curl_off_t,
                     curl_off_t) {
  return static_cast<Transfer*>(context)->cancel.load() ? 1 : 0;
}
bool IsTrustedScreenScraperUrl(std::string_view url) {
  if (!url.starts_with("https://")) {
    return false;
  }
  auto host = url.substr(8, url.find('/', 8) - 8);
  if (host.find_first_of("@:\\?#") != host.npos) {
    return false;
  }
  return host == "screenscraper.fr" || host == "api.screenscraper.fr" ||
         host == "www.screenscraper.fr" || host.ends_with(".screenscraper.fr");
}
const char* RequestKind(std::string_view url) {
  const auto path_start = url.find('/', 8);
  if (path_start != url.npos) {
    auto path = url.substr(path_start);
    path = path.substr(0, path.find('?'));
    if (path == "/api2/ssuserInfos.php") {
      return "account";
    }
    if (path == "/api2/jeuRecherche.php") {
      return "search";
    }
    if (path == "/api2/jeuInfos.php") {
      return "game metadata";
    }
    if (path == "/api2/mediaJeu.php") {
      return "image";
    }
    if (path == "/api2/mediaVideoJeu.php") {
      return "video";
    }
    if (path == "/api2/mediaManuelJeu.php") {
      return "manual";
    }
  }
  return "request";
}
}  // namespace
Transport MakeHttpTransport() {
  return [](const std::string& requested, size_t limit,
            const std::atomic<bool>& cancel) {
    Transfer transfer{{}, limit, cancel};
    auto url = requested;
    const auto kind = RequestKind(requested);
    const auto started = std::chrono::steady_clock::now();
    if (!cancel) {
      XELOGI("ScreenScraper: {} request started", kind);
    }
    // Keep authenticated redirects on ScreenScraper HTTPS endpoints.
    for (unsigned redirect = 0; redirect < 4 && !cancel; ++redirect) {
      if (!IsTrustedScreenScraperUrl(url)) {
        XELOGE(
            "ScreenScraper: {} request rejected an untrusted URL or redirect",
            kind);
        transfer.response = {};
        break;
      }
      CURL* handle = CreateCurlHandle();
      if (!handle) {
        XELOGE("ScreenScraper: {} request could not initialize HTTPS", kind);
        break;
      }
      transfer.response = {};
      transfer.limit_exceeded = false;
      curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt(handle, CURLOPT_TIMEOUT, 90L);
      curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
      curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 15L);
      curl_easy_setopt(handle, CURLOPT_USERAGENT,
                       "Xenia-Canary-ScreenScraper/1");
      curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, Receive);
      curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
      curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, Header);
      curl_easy_setopt(handle, CURLOPT_HEADERDATA, &transfer);
      curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, TransferProgress);
      curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
      curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
      const auto result = curl_easy_perform(handle);
      curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE,
                        &transfer.response.status);
      char* next = nullptr;
      curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &next);
      const std::string redirect_url = next ? next : "";
      curl_easy_cleanup(handle);
      if (result != CURLE_OK) {
        if (!cancel) {
          XELOGE(
              "ScreenScraper: {} request failed (curl {}: {}, HTTP {}, "
              "{} bytes, size limit exceeded: {})",
              kind, static_cast<int>(result), curl_easy_strerror(result),
              transfer.response.status, transfer.response.body.size(),
              transfer.limit_exceeded);
        }
        transfer.response.status = 0;
        transfer.response.body.clear();
        break;
      }
      if (transfer.response.status < 300 || transfer.response.status >= 400 ||
          redirect_url.empty()) {
        break;
      }
      url = redirect_url;
    }
    transfer.response.cancelled = cancel;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    XELOGI(
        "ScreenScraper: {} request finished (HTTP {}, {} bytes, {} ms, "
        "cancelled: {})",
        kind, transfer.response.status, transfer.response.body.size(), elapsed,
        transfer.response.cancelled);
    return transfer.response;
  };
}
}  // namespace xe::app::screenscraper
