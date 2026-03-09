#include "stt_ws_socket.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <random>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

static std::string base64_encode(const std::vector<unsigned char>& data) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val=0, valb=-6;
    for (unsigned char c : data) {
        val = (val<<8) + c;
        valb += 8;
        while (valb>=0) {
            out.push_back(b64[(val>>valb)&0x3F]);
            valb-=6;
        }
    }
    if (valb>-6) out.push_back(b64[((val<<8)>>(valb+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}

STTWebSocket::STTWebSocket() {}
STTWebSocket::~STTWebSocket() { Close(); }

bool STTWebSocket::Connect(const std::wstring& hostW, unsigned short port, const std::wstring& pathW) {
    if (running_) return false;
    std::string host;
    std::string path;
    // convert
    int hlen = WideCharToMultiByte(CP_UTF8,0,hostW.c_str(),-1,NULL,0,NULL,NULL);
    host.resize(hlen);
    WideCharToMultiByte(CP_UTF8,0,hostW.c_str(),-1,&host[0],hlen,NULL,NULL);
    int plen = WideCharToMultiByte(CP_UTF8,0,pathW.c_str(),-1,NULL,0,NULL,NULL);
    path.resize(plen);
    WideCharToMultiByte(CP_UTF8,0,pathW.c_str(),-1,&path[0],plen,NULL,NULL);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = NULL;
    std::stringstream ss;
    ss << port;
    if (getaddrinfo(host.c_str(), ss.str().c_str(), &hints, &result) != 0) {
        WSACleanup();
        return false;
    }

    sock_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock_ == INVALID_SOCKET) {
        freeaddrinfo(result);
        WSACleanup();
        return false;
    }
    if (connect(sock_, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        freeaddrinfo(result);
        WSACleanup();
        return false;
    }
    freeaddrinfo(result);

    std::vector<unsigned char> key(16);
    std::random_device rd;
    for (int i=0;i<16;i++) key[i] = (unsigned char)(rd() & 0xFF);
    std::string secKey = base64_encode(key);

    if (!DoHandshake(host, port, path, secKey)) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    running_ = true;
    // start recv thread
    DWORD tid = 0;
    recvThread_ = (HANDLE)_beginthreadex(NULL, 0, (unsigned int(__stdcall*)(void*))RecvThreadProc, this, 0, (unsigned*)&tid);
    return true;
}

bool STTWebSocket::DoHandshake(const std::string& host, unsigned short port, const std::string& path, std::string& secKey) {
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "Sec-WebSocket-Key: " << secKey << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";
    req << "\r\n";
    std::string s = req.str();
    int sent = send(sock_, s.c_str(), (int)s.size(), 0);
    if (sent == SOCKET_ERROR) return false;
    // read response header
    char buf[4096];
    int r = recv(sock_, buf, sizeof(buf)-1, 0);
    if (r <= 0) return false;
    buf[r] = '\0';
    std::string resp(buf);
    if (resp.find("101") == std::string::npos) return false;
    return true;
}

unsigned int STTWebSocket::RecvThreadProc(void* param) {
    STTWebSocket* self = (STTWebSocket*)param;
    self->RunRecvLoop();
    return 0;
}

void STTWebSocket::RunRecvLoop() {
    const int BUF = 8192;
    std::vector<char> buf(BUF);
    while (running_) {
        int r = recv(sock_, buf.data(), BUF, 0);
        if (r <= 0) break;
        // parse simple text frame(s). Assume server sends small text frames without fragmentation.
        unsigned char b0 = (unsigned char)buf[0];
        unsigned char opcode = b0 & 0x0F;
        unsigned char b1 = (unsigned char)buf[1];
        size_t len = b1 & 0x7F;
        size_t offset = 2;
        if (len == 126) { len = (unsigned char)buf[2]<<8 | (unsigned char)buf[3]; offset = 4; }
        else if (len == 127) { // not expected
            len = 0;
        }
        if (opcode == 0x1) { // text
            std::string msg(buf.data()+offset, buf.data()+offset+len);
            // post to HWND
            if (targetHwnd_) {
                int wlen = MultiByteToWideChar(CP_UTF8,0,msg.c_str(),-1,NULL,0);
                if (wlen>0) {
                    wchar_t* wb = new wchar_t[wlen];
                    MultiByteToWideChar(CP_UTF8,0,msg.c_str(),-1,wb,wlen);
                    PostMessageW(targetHwnd_, WM_APP+1, 0, reinterpret_cast<LPARAM>(wb));
                }
            }
        }
    }
}

bool STTWebSocket::SendBinary(const char* data, size_t len) {
    if (sock_ == INVALID_SOCKET) return false;
    // Build a single-frame masked binary message
    std::vector<char> frame;
    unsigned char b0 = 0x82; // FIN=1, opcode=2
    frame.push_back((char)b0);
    if (len <= 125) {
        frame.push_back((char)(0x80 | (unsigned char)len)); // MASK bit set
    } else if (len <= 65535) {
        frame.push_back((char)(0x80 | 126));
        unsigned short l = htons((unsigned short)len);
        frame.insert(frame.end(), (char*)&l, (char*)&l + 2);
    } else {
        // not handling >32-bit lengths for prototype
        return false;
    }
    // mask
    unsigned char mask[4];
    std::random_device rd;
    for (int i=0;i<4;i++) mask[i] = (unsigned char)(rd() & 0xFF);
    frame.insert(frame.end(), (char*)mask, (char*)mask+4);
    // masked payload
    for (size_t i=0;i<len;i++) {
        char c = data[i] ^ mask[i%4];
        frame.push_back(c);
    }
    int sent = send(sock_, frame.data(), (int)frame.size(), 0);
    return sent == (int)frame.size();
}

void STTWebSocket::SetTargetWindow(HWND hwnd) { targetHwnd_ = hwnd; }

void STTWebSocket::Close() {
    if (!running_) return;
    running_ = false;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (recvThread_) {
        WaitForSingleObject(recvThread_, 2000);
        CloseHandle(recvThread_);
        recvThread_ = NULL;
    }
    WSACleanup();
}
