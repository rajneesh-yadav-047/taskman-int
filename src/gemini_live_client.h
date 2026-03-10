#pragma once
#include <windows.h>
#include <string>
#include "audio_playback.h"

// For SChannel
#define SECURITY_WIN32
#include <schannel.h>
#include <security.h>

class GeminiLiveClient {
public:
    GeminiLiveClient();
    ~GeminiLiveClient();

    bool Connect(const std::string& api_key);
    void Close();
    bool SendAudio(const char* data, size_t len);
    bool SendTextPrompt(const std::string& prompt);
    void SetTargetWindow(HWND hwnd);
    void SetAudioPlayer(AudioPlayback* player);

private:
    SOCKET sock_ = INVALID_SOCKET;
    HWND targetHwnd_ = NULL;
    volatile bool running_ = false;
    HANDLE recvThread_ = NULL;
    AudioPlayback* audioPlayer_ = nullptr;
    std::vector<char> m_enc_buffer; // Buffer for encrypted data from socket
    std::vector<char> m_dec_buffer; // Buffer for decrypted data to be consumed

    // SChannel members
    CredHandle hCred_{};
    CtxtHandle hCtxt_{};
    bool tls_established_ = false;
    SecPkgContext_StreamSizes streamSizes_{};

    bool DoTlsHandshake(const std::string& host);
    int SendTls(const char* data, int len);
    int RecvTls(char* buffer, int len);
    bool SendWsTextFrame(const std::string& payload);
    bool DoWsHandshake(const std::string& host, unsigned short port, const std::string& path, std::string& secKey);
    static unsigned int RecvThreadProc(void* param);
    void RunRecvLoop();
};