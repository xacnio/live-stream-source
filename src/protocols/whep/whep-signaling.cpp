// whep-signaling.cpp
#include "protocols/whep/whep-signaling.h"
#include "core/common.h"

#include <cstring>
#include <vector>

// We use WinHTTP on Windows for simplicity (no extra deps).
// Could use libcurl or FFmpeg avio in the future.
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace lss {

#ifdef _WIN32

// Helper: parse URL into components
struct UrlParts {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = 443;
  bool secure = true;
};

static bool parse_url(const std::string &url, UrlParts &parts) {
  int wlen =
      MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), nullptr, 0);
  std::wstring wurl(wlen, 0);
  MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(), &wurl[0], wlen);

  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof(uc);

  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;

  if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
    return false;

  parts.host = host;
  parts.path = path;
  parts.port = uc.nPort;
  parts.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  return true;
}

// perform HTTP request
struct HttpResponse {
  int status = 0;
  std::string body;
  std::string location; // Location header
};

static HttpResponse http_request(const std::string &method,
                                 const std::string &url,
                                 const std::string &body,
                                 const std::string &content_type,
                                 const std::string &bearer_token) {
  HttpResponse resp;

  UrlParts parts;
  if (!parse_url(url, parts)) {
    resp.status = -1;
    return resp;
  }

  int mlen = MultiByteToWideChar(CP_UTF8, 0, method.c_str(), (int)method.size(),
                                 nullptr, 0);
  std::wstring wmethod(mlen, 0);
  MultiByteToWideChar(CP_UTF8, 0, method.c_str(), (int)method.size(),
                      &wmethod[0], mlen);

  HINTERNET session = WinHttpOpen(
      L"LIVE-STREAM-SOURCE-WHEP/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    return resp;

  HINTERNET connect =
      WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    return resp;
  }

  DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(
      connect, wmethod.c_str(), parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return resp;
  }

  std::wstring headers;
  if (!content_type.empty()) {
    headers += L"Content-Type: ";
    int ctlen = MultiByteToWideChar(CP_UTF8, 0, content_type.c_str(),
                                    (int)content_type.size(), nullptr, 0);
    std::wstring wct(ctlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, content_type.c_str(),
                        (int)content_type.size(), &wct[0], ctlen);
    headers += wct;
    headers += L"\r\n";
  }

  if (!bearer_token.empty()) {
    headers += L"Authorization: Bearer ";
    int btlen = MultiByteToWideChar(CP_UTF8, 0, bearer_token.c_str(),
                                    (int)bearer_token.size(), nullptr, 0);
    std::wstring wbt(btlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, bearer_token.c_str(),
                        (int)bearer_token.size(), &wbt[0], btlen);
    headers += wbt;
    headers += L"\r\n";
  }

  if (!headers.empty()) {
    WinHttpAddRequestHeaders(request, headers.c_str(), (DWORD)headers.size(),
                             WINHTTP_ADDREQ_FLAG_ADD);
  }

  LPVOID send_data =
      body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str();
  DWORD send_len = body.empty() ? 0 : (DWORD)body.size();

  BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               send_data, send_len, send_len, 0);
  if (!ok) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return resp;
  }

  ok = WinHttpReceiveResponse(request, nullptr);
  if (!ok) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return resp;
  }

  DWORD status_code = 0;
  DWORD sz = sizeof(status_code);
  WinHttpQueryHeaders(
      request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sz, WINHTTP_NO_HEADER_INDEX);
  resp.status = (int)status_code;

  DWORD loc_sz = 0;
  WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                      WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &loc_sz,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && loc_sz > 0) {
    std::vector<wchar_t> loc_buf(loc_sz / sizeof(wchar_t) + 1, 0);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX, loc_buf.data(),
                            &loc_sz, WINHTTP_NO_HEADER_INDEX)) {
      int utf8_len = WideCharToMultiByte(CP_UTF8, 0, loc_buf.data(), -1,
                                         nullptr, 0, nullptr, nullptr);
      resp.location.resize(utf8_len - 1);
      WideCharToMultiByte(CP_UTF8, 0, loc_buf.data(), -1, &resp.location[0],
                          utf8_len, nullptr, nullptr);
    }
  }

  std::string response_body;
  DWORD bytes_available = 0;
  do {
    bytes_available = 0;
    WinHttpQueryDataAvailable(request, &bytes_available);
    if (bytes_available > 0) {
      std::vector<char> buf(bytes_available);
      DWORD bytes_read = 0;
      if (WinHttpReadData(request, buf.data(), bytes_available, &bytes_read)) {
        response_body.append(buf.data(), bytes_read);
      }
    }
  } while (bytes_available > 0);

  resp.body = std::move(response_body);

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return resp;
}

#endif // _WIN32

//  WHEP Offer (POST)

WhepSession WhepSignaling::offer(const std::string &whep_url,
                                 const std::string &sdp_offer,
                                 const std::string &bearer_token) {
  WhepSession session;

  lss_log_debug("[WHEP] POST offer to: %s", whep_url.c_str());
  lss_log_debug("[WHEP] SDP offer length: %zu bytes", sdp_offer.size());

#ifdef _WIN32
  auto resp = http_request("POST", whep_url, sdp_offer, "application/sdp",
                           bearer_token);
  session.http_status = resp.status;

  if (resp.status == 201 || resp.status == 200) {
    session.sdp_answer = resp.body;
    session.resource_url = resp.location;

    // If Location is relative, resolve against the WHEP URL
    if (!session.resource_url.empty() && session.resource_url[0] == '/') {
      // Extract scheme + host from whep_url
      size_t scheme_end = whep_url.find("://");
      if (scheme_end != std::string::npos) {
        size_t host_end = whep_url.find('/', scheme_end + 3);
        std::string base = (host_end != std::string::npos)
                               ? whep_url.substr(0, host_end)
                               : whep_url;
        session.resource_url = base + session.resource_url;
      }
    }

    lss_log_debug("[WHEP] Got SDP answer (%zu bytes), resource: %s",
                  session.sdp_answer.size(), session.resource_url.c_str());
  } else {
    session.error = "HTTP " + std::to_string(resp.status);
    lss_log_error("[WHEP] Offer failed: HTTP %d, body: %.200s", resp.status,
                  resp.body.c_str());
  }
#else
  session.error = "WHEP signaling not implemented for this platform";
  session.http_status = -1;
#endif

  return session;
}

//  WHEP Teardown (DELETE)

bool WhepSignaling::teardown(const std::string &resource_url,
                             const std::string &bearer_token) {
  if (resource_url.empty()) {
    lss_log_warn("[WHEP] No resource URL for teardown");
    return false;
  }

  lss_log_debug("[WHEP] DELETE teardown: %s", resource_url.c_str());

#ifdef _WIN32
  auto resp = http_request("DELETE", resource_url, "", "", bearer_token);
  bool ok = (resp.status >= 200 && resp.status < 300);
  if (ok) {
    lss_log_debug("[WHEP] Teardown successful (HTTP %d)", resp.status);
  } else {
    lss_log_warn("[WHEP] Teardown failed (HTTP %d)", resp.status);
  }
  return ok;
#else
  return false;
#endif
}

} // namespace lss
