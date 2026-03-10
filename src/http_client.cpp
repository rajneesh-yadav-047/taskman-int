#include "http_client.h"
#include <windows.h>
#include <winhttp.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h" // For GEMINI_API_KEY

#pragma comment(lib, "winhttp.lib")

// Very small URL parser for http://host:port/path
static void parse_url(const std::string& url, std::wstring& host, INTERNET_PORT& port, std::wstring& path, bool& use_https) {
    use_https = false;
    std::string working = url;
    if (working.rfind("http://", 0) == 0) {
        working = working.substr(7);
        use_https = false;
        port = INTERNET_DEFAULT_HTTP_PORT;
    } else if (working.rfind("https://", 0) == 0) {
        working = working.substr(8);
        use_https = true;
        port = INTERNET_DEFAULT_HTTPS_PORT;
    }

    size_t pos = working.find('/');
    std::string hostpart = (pos == std::string::npos) ? working : working.substr(0, pos);
    path = (pos == std::string::npos) ? L"/" : std::wstring(working.begin()+pos, working.end());

    size_t colon = hostpart.find(':');
    std::string hostname = hostpart;
    if (colon != std::string::npos) {
        hostname = hostpart.substr(0, colon);
        port = (INTERNET_PORT)std::stoi(hostpart.substr(colon+1));
    }

    host = std::wstring(hostname.begin(), hostname.end());
}

std::string http_post_json(const std::string& url, const std::string& json) {
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;
    bool use_https = false;
    parse_url(url, host, port, path, use_https);

    // Open WinHTTP session; do not force NO_PROXY here because system proxy is usually fine for remote calls
    HINTERNET hSession = WinHttpOpen(L"taskman-int/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        DWORD err = GetLastError();
        throw std::runtime_error(std::string("WinHttpOpen failed: ") + std::to_string(err));
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpConnect failed"); }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, use_https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpOpenRequest failed"); }

    // Set headers. For direct-to-API calls, we may need to add an Authorization header.
    std::wstring headers = L"Content-Type: application/json";
    // Keys starting with "AIza" are sent as a query parameter by the caller.
    // Other keys (like GCP service account tokens) are sent as a Bearer token.
    if (!GEMINI_API_KEY.empty() && GEMINI_API_KEY.rfind("AIza", 0) != 0) {
        int klen = MultiByteToWideChar(CP_UTF8, 0, GEMINI_API_KEY.c_str(), -1, NULL, 0);
        if (klen > 0) {
            std::wstring wkey(klen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, GEMINI_API_KEY.c_str(), -1, &wkey[0], klen);
            if (!wkey.empty() && wkey.back() == L'\0') wkey.pop_back();
            headers += L"\r\nAuthorization: Bearer " + wkey;
        }
    }

    if (!WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1L, WINHTTP_ADDREQ_FLAG_ADD)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        throw std::runtime_error(std::string("WinHttpAddRequestHeaders failed: ") + std::to_string(err));
    }

    BOOL res = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)json.c_str(), (DWORD)json.length(), (DWORD)json.length(), 0);
    if (!res) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        throw std::runtime_error(std::string("WinHttpSendRequest failed: ") + std::to_string(err));
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpReceiveResponse failed"); }

    std::string out;
    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        std::vector<char> buffer(dwSize+1);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) break;
        out.append(buffer.data(), dwDownloaded);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return out;
}
