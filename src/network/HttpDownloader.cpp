#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

#include "CrtBundle.generated.h"

// IDF's bundle callback (non-static in esp_crt_bundle.c but not declared in its header).
extern "C" int esp_crt_verify_callback(void* buf, mbedtls_x509_crt* crt, int depth, uint32_t* flags);
#include "activities/RenderLock.h"

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string resolveRedirectUrl(const std::string& base, const std::string& redirect) {
  if (redirect.rfind("http://", 0) == 0 || redirect.rfind("https://", 0) == 0) {
    return redirect;
  }
  if (redirect.rfind("//", 0) == 0) {
    size_t schemeEnd = base.find("://");
    if (schemeEnd != std::string::npos) {
      return base.substr(0, schemeEnd + 1) + redirect;
    }
    return "https:" + redirect;
  }
  if (redirect.rfind("/", 0) == 0) {
    size_t schemeEnd = base.find("://");
    if (schemeEnd != std::string::npos) {
      size_t hostEnd = base.find("/", schemeEnd + 3);
      if (hostEnd != std::string::npos) {
        return base.substr(0, hostEnd) + redirect;
      }
      return base + redirect;
    }
    return redirect;
  }
  size_t lastSlash = base.find_last_of("/");
  size_t schemeEnd = base.find("://");
  if (schemeEnd != std::string::npos && lastSlash > schemeEnd + 2) {
    return base.substr(0, lastSlash + 1) + redirect;
  }
  return base + "/" + redirect;
}

// --- TLS trust -------------------------------------------------------------
// The IDF verify callback only ever checks a certificate's *issuer* against the
// bundle, so a Let's Encrypt chain (leaf <- YR1 <- Root YR <- ISRG Root X1) still
// ends in an RSA-4096 signature check that runs out of memory at the handshake
// peak on the ESP32-C3. This wrapper first asks whether the certificate itself
// (subject + public key) is in our bundle; if so it is a trust anchor and no
// further signature work is needed. Everything else falls through to IDF.
bool bundleContains(const uint8_t* subject, size_t subjectLen, const uint8_t* key, size_t keyLen) {
  uint32_t firstOffset = 0;
  memcpy(&firstOffset, CRT_BUNDLE, sizeof(firstOffset));
  const uint32_t count = firstOffset / sizeof(uint32_t);
  uint32_t lo = 0;
  uint32_t hi = count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    uint32_t offset = 0;
    memcpy(&offset, CRT_BUNDLE + mid * sizeof(uint32_t), sizeof(offset));
    uint16_t nameLen = 0;
    uint16_t keyLen2 = 0;
    memcpy(&nameLen, CRT_BUNDLE + offset, sizeof(nameLen));
    memcpy(&keyLen2, CRT_BUNDLE + offset + 2, sizeof(keyLen2));
    const uint8_t* name = CRT_BUNDLE + offset + 4;
    const size_t common = std::min<size_t>(nameLen, subjectLen);
    int cmp = memcmp(name, subject, common);
    if (cmp == 0) cmp = (nameLen < subjectLen) ? -1 : (nameLen > subjectLen ? 1 : 0);
    if (cmp == 0) {
      return keyLen2 == keyLen && memcmp(name + nameLen, key, keyLen) == 0;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

int verifyWithAnchors(void* buf, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
  const uint32_t filtered = *flags & ~static_cast<uint32_t>(MBEDTLS_X509_BADCERT_BAD_MD);
  if (filtered == MBEDTLS_X509_BADCERT_NOT_TRUSTED &&
      bundleContains(crt->subject_raw.p, crt->subject_raw.len, crt->pk_raw.p, crt->pk_raw.len)) {
    LOG_DBG("HTTP", "TLS: trust anchor matched at depth %d", depth);
    *flags = 0;
    return 0;
  }
  return esp_crt_verify_callback(buf, crt, depth, flags);
}

esp_err_t attachTrustBundle(void* conf) {
  static bool bundleInstalled = false;
  if (!bundleInstalled) {
    bundleInstalled = esp_crt_bundle_set(CRT_BUNDLE, sizeof(CRT_BUNDLE)) == ESP_OK;
    if (!bundleInstalled) LOG_ERR("HTTP", "Custom CA bundle rejected, using default");
  }
  const esp_err_t err = esp_crt_bundle_attach(conf);
  if (err == ESP_OK) {
    mbedtls_ssl_conf_verify(static_cast<mbedtls_ssl_config*>(conf), verifyWithAnchors, nullptr);
  }
  return err;
}

// esp_http_client_get_header() only reads *request* headers, so the redirect
// target must be captured from the response header event instead.
struct ResponseCapture {
  std::string location;
};

esp_err_t captureResponseHeaders(esp_http_client_event_t* evt) {
  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data && evt->header_key && evt->header_value &&
      strcasecmp(evt->header_key, "Location") == 0) {
    static_cast<ResponseCapture*>(evt->user_data)->location = evt->header_value;
  }
  return ESP_OK;
}

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink, std::string* outContentType = nullptr,
                                     std::string* outFinalUrl = nullptr, std::string* outErrorDetail = nullptr,
                                     const std::string& bearerToken = "") {
  std::string currentUrl = url;
  int hop = 0;
  esp_http_client_handle_t client = nullptr;
  esp_err_t err = ESP_OK;
  int status = 0;
  int64_t contentLength = 0;

  while (hop < 10) {
    ResponseCapture capture;
    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    config.event_handler = captureResponseHeaders;
    config.user_data = &capture;
    config.buffer_size = HTTP_RX_BUF;
    config.buffer_size_tx = HTTP_TX_BUF;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    // Verify HTTPS against our bundle (roots + intermediates as trust anchors).
    config.crt_bundle_attach = attachTrustBundle;  // roots + intermediates as trust anchors, see above
    config.keep_alive_enable = true;
    // The device never gets a routable IPv6 address; resolving AAAA first can stall connects.
    config.addr_type = HTTP_ADDR_TYPE_INET;

    client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "client init failed");
      if (outErrorDetail) *outErrorDetail = "Client init failed";
      return HttpDownloader::HTTP_ERROR;
    }

    esp_http_client_set_header(client, "User-Agent",
                               "CrossPointReader/1.0 (https://github.com/zakerytclarke/crosspoint-reader-apps)");
    if (!username.empty() && !password.empty()) {
      const std::string credentials = username + ":" + password;
      const String header = "Basic " + base64::encode(credentials.c_str());
      esp_http_client_set_header(client, "Authorization", header.c_str());
    } else if (!bearerToken.empty()) {
      const std::string header = "Bearer " + bearerToken;
      esp_http_client_set_header(client, "Authorization", header.c_str());
    }

    const unsigned long openStart = millis();
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      const int sockErrno = esp_http_client_get_errno(client);
      LOG_ERR("HTTP", "open failed: %s errno=%d after %lu ms heap=%u", esp_err_to_name(err), sockErrno,
              millis() - openStart, static_cast<unsigned>(ESP.getFreeHeap()));
      if (outErrorDetail) {
        *outErrorDetail = std::string("Open failed: ") + esp_err_to_name(err) + " errno " + std::to_string(sockErrno);
      }
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    LOG_INF("HTTP", "%s -> %d in %lu ms, len %lld, heap %u", currentUrl.c_str(), status, millis() - openStart,
            static_cast<long long>(contentLength), static_cast<unsigned>(ESP.getFreeHeap()));

    if (isRedirect(status)) {
      if (!capture.location.empty()) {
        std::string nextUrl = resolveRedirectUrl(currentUrl, capture.location.c_str());
        LOG_DBG("HTTP", "Redirecting from %s to: %s", currentUrl.c_str(), nextUrl.c_str());
        currentUrl = nextUrl;
        esp_http_client_cleanup(client);
        client = nullptr;
        hop++;
        continue;
      }
    }

    break;
  }

  if (!client) {
    if (outErrorDetail && outErrorDetail->empty()) *outErrorDetail = "Too many redirects";
    return HttpDownloader::HTTP_ERROR;
  }

  if (outFinalUrl) {
    *outFinalUrl = currentUrl;
  }
  if (outContentType) {
    char* ctype = nullptr;
    esp_http_client_get_header(client, "Content-Type", &ctype);
    if (ctype) {
      *outContentType = ctype;
    } else {
      outContentType->clear();
    }
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    if (outErrorDetail) *outErrorDetail = "HTTP Status: " + std::to_string(status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    if (outErrorDetail) *outErrorDetail = "Out of memory";
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      if (outErrorDetail) *outErrorDetail = "Read error";
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      if (outErrorDetail) *outErrorDetail = "File write failed";
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    if (outErrorDetail) *outErrorDetail = "Incomplete response";
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrlBearer(const std::string& url, const std::string& bearerToken,
                                    const DataCallback& onData) {
  LOG_DBG("HTTP", "Fetching (bearer): %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, "", "", sink, nullptr, nullptr, nullptr, bearerToken) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             std::string* outContentType, std::string* outFinalUrl,
                                                             std::string* outErrorDetail) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  {
    RenderLock lock;
    if (Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
  }
  HalFile file;
  {
    RenderLock lock;
    if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
      LOG_ERR("HTTP", "Failed to open file for writing");
      return FILE_ERROR;
    }
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) {
    RenderLock lock;
    return file.write(data, len) == len;
  };

  const DownloadError result = runGet(url, username, password, sink, outContentType, outFinalUrl, outErrorDetail);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  {
    RenderLock lock;
    file.close();
  }

  if (result != OK) {
    RenderLock lock;
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    if (outErrorDetail) *outErrorDetail = "No data received";
    RenderLock lock;
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
