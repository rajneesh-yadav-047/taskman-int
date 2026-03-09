#pragma once
#include <windows.h>
#include <string>

// Minimal WebSocket client using Winsock for binary streaming to STT endpoint.
// Not a full-featured implementation — sufficient to connect, send binary frames,
// and receive text frames from the server (used for prototyping).

class STTWebSocket {
public:
    STTWebSocket();
    ~STTWebSocket();

    // Connect to ws://host:port/path. Returns true on success.
    bool Connect(const std::wstring& host, unsigned short port, const std::wstring& path);
    // Send binary data (masked as required by WebSocket client frames).
    bool SendBinary(const char* data, size_t len);
    // Receive thread posts text frames via PostMessage(hwnd, WM_APP+1, 0, LPARAM(wchar_t*))
    // Set the window handle to post transcripts to.
    void SetTargetWindow(HWND hwnd);
    // Close connection
    void Close();

private:
    SOCKET sock_ = INVALID_SOCKET;
    HWND targetHwnd_ = NULL;
    volatile bool running_ = false;
    HANDLE recvThread_ = NULL;

    bool DoHandshake(const std::string& host, unsigned short port, const std::string& path, std::string& secKey);
    static unsigned int RecvThreadProc(void* param);
    void RunRecvLoop();
};
