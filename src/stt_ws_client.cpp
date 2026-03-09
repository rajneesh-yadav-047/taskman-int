#include "stt_ws_client.h"
#include <winhttp.h>
#include <fstream>
#include <vector>
#include <memory>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

// Helper to post a UTF-8 std::string to hwnd as WM_APP+1 (allocates wchar_t* on heap)
static void PostUtf8ToWindow(HWND hwnd, const std::string& utf8) {
    if (!hwnd) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t* wbuf = new wchar_t[wlen];
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wbuf, wlen);
    PostMessageW(hwnd, WM_APP + 1, 0, reinterpret_cast<LPARAM>(wbuf));
}

bool stream_file_to_stt_ws(const std::wstring& host, INTERNET_PORT port, const std::wstring& path, const std::string& filepath, HWND hwnd) {
    // Read file into memory (simple for prototype)
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs) return false;
    std::vector<char> filebuf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    // simple debug logging
    {
        std::ofstream log("stt_debug.log", std::ios::app);
        log << "stream_file_to_stt_ws: read file " << filepath << " size=" << filebuf.size() << "\n";
    }

    HINTERNET hSession = WinHttpOpen(L"stt-client/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    {
        std::ofstream log("stt_debug.log", std::ios::app);
        log << "WinHttpOpen: " << (hSession ? "ok" : "fail") << "\n";
    }
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    {
        std::ofstream log("stt_debug.log", std::ios::app);
        log << "WinHttpConnect: " << (hConnect ? "ok" : "fail") << "\n";
    }
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    {
        std::ofstream log("stt_debug.log", std::ios::app);
        log << "WinHttpOpenRequest: " << (hRequest ? "ok" : "fail") << "\n";
    }
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // Request WebSocket upgrade headers
    const wchar_t* upgradeHeaders = L"Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n";
    BOOL ok = WinHttpSendRequest(hRequest, upgradeHeaders, (DWORD)wcslen(upgradeHeaders), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    {
        std::ofstream log("stt_debug.log", std::ios::app);
        log << "WinHttpSendRequest: " << (ok ? "ok" : "fail") << "\n";
    }
    if (!ok) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    ok = WinHttpReceiveResponse(hRequest, NULL);
    {
        std::ofstream log("stt_debug.log", std::ios::app);
        log << "WinHttpReceiveResponse: " << (ok ? "ok" : "fail") << "\n";
    }
    if (!ok) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    {
        std::ofstream log("stt_debug.log", std::ios::app);
        log << "WinHttpWebSocketCompleteUpgrade: " << (hWebSocket ? "ok" : "fail") << "\n";
    }
    if (!hWebSocket) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // Start a receiving thread to collect server transcripts
    std::unique_ptr<std::thread> recvThread(new std::thread([hWebSocket, hwnd]() {
        const DWORD BUFSIZE = 8192;
        std::vector<char> recvBuf(BUFSIZE);
        while (true) {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            HRESULT hr = WinHttpWebSocketReceive(hWebSocket, reinterpret_cast<unsigned char*>(recvBuf.data()), (DWORD)recvBuf.size(), &bytesRead, &type);
            if (FAILED(hr)) break;
            if (bytesRead == 0) {
                // remote closed
                break;
            }
            // Treat incoming as text (JSON) for transcripts
            std::string msg(recvBuf.data(), recvBuf.data() + bytesRead);
            PostUtf8ToWindow(hwnd, msg);
        }
    }));

    // Send file in chunks
    const size_t CHUNK = 8192;
    size_t sent = 0;
    while (sent < filebuf.size()) {
        size_t toSend = std::min(CHUNK, filebuf.size() - sent);
        HRESULT hr = WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, reinterpret_cast<void*>(filebuf.data() + sent), (DWORD)toSend);
        if (FAILED(hr)) break;
        sent += toSend;
        // small throttle so the stub can reply
        Sleep(5);
    }

    // Close websocket gracefully
    WinHttpWebSocketClose(hWebSocket, 1000, NULL, 0);

    // wait for recv thread to finish
    if (recvThread && recvThread->joinable()) recvThread->join();

    WinHttpCloseHandle(hWebSocket);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return true;
}
