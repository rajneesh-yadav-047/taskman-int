#include <winsock2.h>
#include <ws2tcpip.h>
#include "stt_ws_socket.h"
#include <wincrypt.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <thread>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

STTWebSocket::STTWebSocket() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}

STTWebSocket::~STTWebSocket() {
    Close();
    WSACleanup();
}

void STTWebSocket::SetTargetWindow(HWND hwnd) {
    targetHwnd_ = hwnd;
}

bool STTWebSocket::DoHandshake(const std::string& host, unsigned short port, const std::string& path, std::string& secKey) {
    // Generate Sec-WebSocket-Key
    BYTE keyData[16];
    HCRYPTPROV hProv;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return false;
    }
    CryptGenRandom(hProv, sizeof(keyData), keyData);
    CryptReleaseContext(hProv, 0);

    // Base64-encode the random 16 bytes to produce the Sec-WebSocket-Key
    DWORD keyLen = 0;
    if (!CryptBinaryToStringA(keyData, sizeof(keyData), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &keyLen)) {
        CryptReleaseContext(hProv, 0);
        return false;
    }
    std::string tmp; tmp.resize(keyLen);
    if (!CryptBinaryToStringA(keyData, sizeof(keyData), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &tmp[0], &keyLen)) {
        CryptReleaseContext(hProv, 0);
        return false;
    }
    // CryptBinaryToStringA writes a null-terminated string; remove any trailing null
    if (!tmp.empty() && tmp.back() == '\0') tmp.pop_back();
    secKey = tmp;
    CryptReleaseContext(hProv, 0);

    std::string request = "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + secKey + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "\r\n";

    if (send(sock_, request.c_str(), (int)request.length(), 0) == SOCKET_ERROR) {
        return false;
    }

    // Read response headers fully (until CRLF CRLF) with a simple timeout.
    std::string resp;
    char buffer[2048];
    int totalWaitMs = 0;
    const int maxWaitMs = 3000;
    const int sleepStepMs = 50;
    // We'll try to recv until we find header terminator or timeout
    while (resp.find("\r\n\r\n") == std::string::npos && totalWaitMs < maxWaitMs) {
        int bytes = recv(sock_, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
            resp.append(buffer, bytes);
            // if headers ended, break
            if (resp.find("\r\n\r\n") != std::string::npos) break;
            // continue reading immediately
            continue;
        } else if (bytes == 0) {
            // connection closed
            break;
        } else {
            // no data available right now; wait a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepStepMs));
            totalWaitMs += sleepStepMs;
        }
    }

    if (resp.empty()) return false;

    // Basic check for HTTP/1.1 101 Switching Protocols
    if (resp.find(" 101 ") == std::string::npos && resp.find("101 Switching Protocols") == std::string::npos) {
        return false;
    }

    // Verify required headers: Upgrade: websocket and Connection: Upgrade
    auto findHeader = [&](const std::string& name)->std::string {
        std::string low = name + ":";
        size_t pos = std::string::npos;
        // case-insensitive search for header name
        std::string tmp = resp;
        // convert tmp to lower-case for header search
        for (auto &c : tmp) c = (char)tolower((unsigned char)c);
        std::string nameLower = name; for (auto &c : nameLower) c = (char)tolower((unsigned char)c);
        pos = tmp.find(nameLower + ":");
        if (pos == std::string::npos) return std::string();
        // find end of line
        size_t lineEnd = tmp.find("\r\n", pos);
        if (lineEnd == std::string::npos) return std::string();
        size_t valStart = tmp.find_first_not_of(" \t:", pos + nameLower.length());
        if (valStart == std::string::npos || valStart >= lineEnd) return std::string();
        std::string val = tmp.substr(valStart, lineEnd - valStart);
        // trim
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ' || val.back() == '\t')) val.pop_back();
        return val;
    };

    std::string upgrade = findHeader("Upgrade");
    std::string connection = findHeader("Connection");
    if (upgrade.find("websocket") == std::string::npos) return false;
    if (connection.find("upgrade") == std::string::npos) return false;

    // Validate Sec-WebSocket-Accept if present
    // Compute expected accept: base64( SHA1( secKey + magic ) )
    std::string acceptHdr;
    // case-insensitive header lookup for Sec-WebSocket-Accept
    // We'll search manually in original resp
    size_t s_pos = resp.find("Sec-WebSocket-Accept:");
    if (s_pos == std::string::npos) s_pos = resp.find("sec-websocket-accept:");
    if (s_pos != std::string::npos) {
        size_t lineEnd = resp.find("\r\n", s_pos);
        if (lineEnd != std::string::npos) {
            size_t valStart = resp.find_first_not_of(" \t:", s_pos + 21);
            if (valStart != std::string::npos && valStart < lineEnd) {
                acceptHdr = resp.substr(valStart, lineEnd - valStart);
                // trim
                while (!acceptHdr.empty() && (acceptHdr.back()=='\r' || acceptHdr.back()=='\n' || acceptHdr.back()==' ')) acceptHdr.pop_back();
            }
        }
    }

    if (!acceptHdr.empty()) {
        // compute expected
        const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string concat = secKey + magic;

        HCRYPTPROV hProv2 = 0;
        if (!CryptAcquireContext(&hProv2, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            // fallback to PROV_RSA_FULL
            if (!CryptAcquireContext(&hProv2, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) hProv2 = 0;
        }
        bool ok = false;
        if (hProv2) {
            HCRYPTHASH hHash = 0;
            if (CryptCreateHash(hProv2, CALG_SHA1, 0, 0, &hHash)) {
                if (CryptHashData(hHash, (const BYTE*)concat.data(), (DWORD)concat.size(), 0)) {
                    DWORD hashLen = 20; // SHA-1
                    std::vector<BYTE> hash(hashLen);
                    if (CryptGetHashParam(hHash, HP_HASHVAL, hash.data(), &hashLen, 0)) {
                        // base64-encode
                        DWORD outLen = 0;
                        if (CryptBinaryToStringA(hash.data(), hashLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &outLen)) {
                            std::string b64; b64.resize(outLen);
                            if (CryptBinaryToStringA(hash.data(), hashLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &outLen)) {
                                if (!b64.empty() && b64.back() == '\0') b64.pop_back();
                                // compare case-sensitively
                                if (b64 == acceptHdr) ok = true;
                            }
                        }
                    }
                }
                CryptDestroyHash(hHash);
            }
            CryptReleaseContext(hProv2, 0);
        }
        if (!ok) return false;
    }

    return true;
}

bool STTWebSocket::Connect(const std::wstring& host, unsigned short port, const std::wstring& path) {
    Close();

    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string sport = std::to_string(port);
    int utf8HostLen = WideCharToMultiByte(CP_UTF8, 0, host.c_str(), -1, NULL, 0, NULL, NULL);
    if (utf8HostLen <= 0) {
        freeaddrinfo(res);
        return false;
    }
    std::string shost(utf8HostLen, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, host.c_str(), -1, &shost[0], utf8HostLen, NULL, NULL);
    if (ret == 0) {
        freeaddrinfo(res);
        return false;
    }
    // ret includes the terminating null, so resize to exclude it
    if (!shost.empty()) shost.resize(ret - 1);

    if (getaddrinfo(shost.c_str(), sport.c_str(), &hints, &res) != 0) {
        return false;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }

    if (connect(sock_, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    int utf8PathLen = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, NULL, 0, NULL, NULL);
    std::string spath(utf8PathLen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &spath[0], utf8PathLen, NULL, NULL);
    spath.pop_back();

    std::string secKey;
    if (!DoHandshake(shost, port, spath, secKey)) {
        Close();
        return false;
    }

    running_ = true;
    recvThread_ = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)RecvThreadProc, this, 0, NULL);
    return true;
}

bool STTWebSocket::SendBinary(const char* data, size_t len) {
    if (sock_ == INVALID_SOCKET || !running_) return false;

    std::vector<char> frame;
    frame.push_back((char)0x82); // FIN + Binary frame

    if (len <= 125) {
        frame.push_back((char)(len | 0x80)); // Masked
    } else if (len <= 65535) {
        frame.push_back((char)(126 | 0x80)); // Masked
        frame.push_back((char)((len >> 8) & 0xFF));
        frame.push_back((char)(len & 0xFF));
    } else {
        frame.push_back((char)(127 | 0x80)); // Masked
        for (int i = 7; i >= 0; --i) {
            frame.push_back((char)(((uint64_t)len >> (i * 8)) & 0xFF));
        }
    }

    // Masking key
    char mask[4];
    for (int i = 0; i < 4; ++i) mask[i] = rand() % 256;
    frame.insert(frame.end(), mask, mask + 4);

    // Mask payload
    size_t payload_start = frame.size();
    frame.insert(frame.end(), data, data + len);
    for (size_t i = 0; i < len; ++i) {
        frame[payload_start + i] ^= mask[i % 4];
    }

    return send(sock_, frame.data(), (int)frame.size(), 0) != SOCKET_ERROR;
}

void STTWebSocket::Close() {
    if (running_) {
        running_ = false;
        if (sock_ != INVALID_SOCKET) {
            shutdown(sock_, SD_BOTH);
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (recvThread_) {
            WaitForSingleObject(recvThread_, 2000);
            CloseHandle(recvThread_);
            recvThread_ = NULL;
        }
    }
}

unsigned int STTWebSocket::RecvThreadProc(void* param) {
    STTWebSocket* pThis = static_cast<STTWebSocket*>(param);
    pThis->RunRecvLoop();
    return 0;
}

void STTWebSocket::RunRecvLoop() {
    std::vector<char> buffer(8192);
    while (running_) {
        int bytes = recv(sock_, buffer.data(), (int)buffer.size(), 0);
        if (bytes <= 0) {
            running_ = false;
            break;
        }

        // Basic frame parsing (assumes unmasked text frames)
        if ((buffer[0] & 0x0F) == 0x01) { // Text frame
            size_t len = (buffer[1] & 0x7F);
            size_t offset = 2;
            if (len == 126) {
                len = (static_cast<unsigned char>(buffer[2]) << 8) | static_cast<unsigned char>(buffer[3]);
                offset = 4;
            } else if (len == 127) {
                // Not handling large frames for this simple client
                continue;
            }

            if (bytes < offset + len) continue; // Incomplete frame

            std::string payload(buffer.data() + offset, len);

            // Check for utterance end event first
            if (payload.find("\"event\":\"utterance_end\"") != std::string::npos) {
                if (targetHwnd_) {
                    PostMessageW(targetHwnd_, WM_APP + 3, 0, 0);
                }
                continue; // This was an event message, not a transcript
            }

            // Manual JSON parsing for transcript and is_final
            std::string transcript;
            bool is_final = false;

            size_t t_pos = payload.find("\"transcript\"");
            if (t_pos != std::string::npos) {
                size_t start_quote = payload.find('"', t_pos + 12);
                if (start_quote != std::string::npos) {
                    size_t end_quote = payload.find('"', start_quote + 1);
                    if (end_quote != std::string::npos) {
                        transcript = payload.substr(start_quote + 1, end_quote - start_quote - 1);
                    }
                }
            }

            size_t f_pos = payload.find("\"is_final\"");
            if (f_pos != std::string::npos) {
                size_t colon = payload.find(':', f_pos);
                if (colon != std::string::npos) {
                    size_t val_start = payload.find_first_not_of(" \t\r\n", colon + 1);
                    if (val_start != std::string::npos && payload.substr(val_start, 4) == "true") {
                        is_final = true;
                    }
                }
            }

            if (!transcript.empty() && targetHwnd_) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, transcript.c_str(), -1, NULL, 0);
                if (wlen > 0) {
                    wchar_t* wbuf = new wchar_t[wlen];
                    MultiByteToWideChar(CP_UTF8, 0, transcript.c_str(), -1, wbuf, wlen);
                    // Post to main thread: WM_APP+2, is_final, text
                    PostMessageW(targetHwnd_, WM_APP + 2, (WPARAM)is_final, (LPARAM)wbuf);
                }
            }
        }
    }
}