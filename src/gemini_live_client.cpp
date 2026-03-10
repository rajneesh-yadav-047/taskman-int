#include <winsock2.h>
#include <ws2tcpip.h>
#include "gemini_live_client.h"
#include <wincrypt.h>
#include "app_messages.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <thread>
#include <iostream>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "secur32.lib")

#ifndef SCH_CRED_USE_STRONG_CRYPTO
#define SCH_CRED_USE_STRONG_CRYPTO 0
#endif

// --- Helper Functions ---

std::string Base64Encode(const char* data, size_t len) {
    if (len == 0) return "";
    DWORD outLen = 0;
    CryptBinaryToStringA((const BYTE*)data, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &outLen);
    if (outLen == 0) return "";
    std::string out(outLen, '\0');
    CryptBinaryToStringA((const BYTE*)data, (DWORD)len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &outLen);
    out.pop_back(); // remove null terminator
    return out;
}

std::vector<char> Base64Decode(const std::string& b64) {
    DWORD outLen = 0;
    CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, NULL, &outLen, NULL, NULL);
    if (outLen == 0) return {};
    std::vector<char> out(outLen);
    CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, (BYTE*)out.data(), &outLen, NULL, NULL);
    return out;
}

std::string json_escape(const std::string& str) {
    std::string escaped;
    escaped.reserve(str.length());
    for (char c : str) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b";  break;
            case '\f': escaped += "\\f";  break;
            case '\n': escaped += "\\n";  break;
            case '\r': escaped += "\\r";  break;
            case '\t': escaped += "\\t";  break;
            default:   escaped += c;      break;
        }
    }
    return escaped;
}

// --- STTWebSocket Class Implementation ---

GeminiLiveClient::GeminiLiveClient() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}

GeminiLiveClient::~GeminiLiveClient() {
    Close();
    // WSACleanup is called once per process, so it's better to do it
    // when the application exits, not in every object destructor.
    // However, for this simple app, it's okay here.
    WSACleanup(); 
}

void GeminiLiveClient::SetTargetWindow(HWND hwnd) {
    targetHwnd_ = hwnd;
}

void GeminiLiveClient::SetAudioPlayer(AudioPlayback* player) {
    audioPlayer_ = player;
}

bool GeminiLiveClient::DoTlsHandshake(const std::string& host) {
    SCHANNEL_CRED cred = {0};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_USE_STRONG_CRYPTO;
    
    TimeStamp expiry;
    SECURITY_STATUS status = AcquireCredentialsHandleA(NULL, (LPSTR)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &hCred_, &expiry);
    if (status != SEC_E_OK) return false;

    SecBuffer outBuf;
    outBuf.pvBuffer = NULL;
    outBuf.cbBuffer = 0;
    outBuf.BufferType = SECBUFFER_TOKEN;

    SecBufferDesc outBufDesc;
    outBufDesc.ulVersion = SECBUFFER_VERSION;
    outBufDesc.cBuffers = 1;
    outBufDesc.pBuffers = &outBuf;

    DWORD dwSSPIFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    
    std::wstring whost(host.begin(), host.end());

    status = InitializeSecurityContextA(&hCred_, NULL, (SEC_CHAR*)host.c_str(), dwSSPIFlags, 0, SECURITY_NATIVE_DREP, NULL, 0, &hCtxt_, &outBufDesc, &dwSSPIFlags, &expiry);

    if (status != SEC_I_CONTINUE_NEEDED) return false;

    if (outBuf.cbBuffer != 0 && outBuf.pvBuffer != NULL) {
        if (send(sock_, (char*)outBuf.pvBuffer, outBuf.cbBuffer, 0) == SOCKET_ERROR) {
            FreeContextBuffer(outBuf.pvBuffer);
            return false;
        }
        FreeContextBuffer(outBuf.pvBuffer);
    }

    std::vector<char> read_buf(8192);
    size_t read_total = 0;

    while (status == SEC_I_CONTINUE_NEEDED) {
        int bytes = recv(sock_, read_buf.data() + read_total, (int)(read_buf.size() - read_total), 0);
        if (bytes <= 0) return false;
        read_total += bytes;

        SecBuffer inBuffers[2];
        inBuffers[0].pvBuffer = read_buf.data();
        inBuffers[0].cbBuffer = read_total;
        inBuffers[0].BufferType = SECBUFFER_TOKEN;
        inBuffers[1].pvBuffer = NULL;
        inBuffers[1].cbBuffer = 0;
        inBuffers[1].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc inBufDesc;
        inBufDesc.ulVersion = SECBUFFER_VERSION;
        inBufDesc.cBuffers = 2;
        inBufDesc.pBuffers = inBuffers;

        outBuf.pvBuffer = NULL;
        outBuf.cbBuffer = 0;
        outBuf.BufferType = SECBUFFER_TOKEN;

        status = InitializeSecurityContextA(&hCred_, &hCtxt_, (SEC_CHAR*)host.c_str(), dwSSPIFlags, 0, SECURITY_NATIVE_DREP, &inBufDesc, 0, &hCtxt_, &outBufDesc, &dwSSPIFlags, &expiry);

        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) {
            if (outBuf.cbBuffer != 0 && outBuf.pvBuffer != NULL) {
                if (send(sock_, (char*)outBuf.pvBuffer, outBuf.cbBuffer, 0) == SOCKET_ERROR) {
                    FreeContextBuffer(outBuf.pvBuffer);
                    return false;
                }
                FreeContextBuffer(outBuf.pvBuffer);
            }
        }

        if (inBuffers[1].BufferType == SECBUFFER_EXTRA) {
            memmove(read_buf.data(), (char*)read_buf.data() + (read_total - inBuffers[1].cbBuffer), inBuffers[1].cbBuffer);
            read_total = inBuffers[1].cbBuffer;
        } else {
            read_total = 0;
        }
    }

    if (status != SEC_E_OK) return false;

    tls_established_ = true;
    QueryContextAttributes(&hCtxt_, SECPKG_ATTR_STREAM_SIZES, &streamSizes_);
    
    // Any leftover data in read_buf is the start of the HTTP response.
    // We need to move it to our decryption buffer.
    if (read_total > 0) {
        dec_buffer_.assign(read_buf.data(), read_buf.data() + read_total);
    }

    return true;
}

bool GeminiLiveClient::DoWsHandshake(const std::string& host, unsigned short port, const std::string& path, std::string& secKey) {
    BYTE keyData[16];
    HCRYPTPROV hProv;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
    CryptGenRandom(hProv, sizeof(keyData), keyData);
    CryptReleaseContext(hProv, 0);

    secKey = Base64Encode((const char*)keyData, sizeof(keyData));

    std::string request = "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + secKey + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "\r\n";

    if (SendTls(request.c_str(), (int)request.length()) <= 0) return false;

    char buffer[2048];
    int bytes = RecvTls(buffer, sizeof(buffer) - 1);
    if (bytes <= 0) { return false; }
    buffer[bytes] = '\0';

    return strstr(buffer, " 101 ") != NULL;
}

bool GeminiLiveClient::Connect(const std::string& api_key) {
    Close();

    // The JS reference implementation uses a specific model for live audio.
    // The streamGenerateContent endpoint supports WebSocket upgrades.
    std::string host = "generativelanguage.googleapis.com";
    unsigned short port = 443;
    std::string model = "gemini-2.5-flash-native-audio-preview-09-2025";
    std::string path = "/v1beta/models/" + model + ":streamGenerateContent?key=" + api_key + "&alt=json";
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }

    if (connect(sock_, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        freeaddrinfo(res);
        if (targetHwnd_) PostMessageW(targetHwnd_, WM_APP + 7, 0, (LPARAM)new std::wstring(L"Status: Connection failed."));
        return false;
    }
    freeaddrinfo(res);

    if (!DoTlsHandshake(host)) {
        Close();
        if (targetHwnd_) PostMessageW(targetHwnd_, WM_APP + 7, 0, (LPARAM)new std::wstring(L"Status: TLS handshake failed."));
        return false;
    }

    std::string secKey;
    if (!DoWsHandshake(host, port, path, secKey)) {
        Close();
        if (targetHwnd_) PostMessageW(targetHwnd_, WM_APP + 7, 0, (LPARAM)new std::wstring(L"Status: WebSocket handshake failed."));
        return false;
    }

    // Send initial configuration frame
    // This is based on the JS reference implementation.
    std::string system_instruction = "You are a passive, helpful listener in a conversation. Listen carefully. If you hear a question that requires an AI's help or a general question directed at you, provide a concise, helpful answer. Otherwise, stay silent and just listen. Do not interrupt the humans unless they ask you something.";
    std::string config_payload = "{\"config\": {\"responseModalities\": [\"AUDIO\"], \"speechConfig\": {\"voiceConfig\": { \"prebuiltVoiceConfig\": { \"voiceName\": \"Zephyr\" } }}, \"systemInstruction\": \"" + json_escape(system_instruction) + "\", \"inputAudioTranscription\": {}, \"outputAudioTranscription\": {}}}";
    if (!SendWsTextFrame(config_payload)) {
        Close();
        if (targetHwnd_) PostMessageW(targetHwnd_, WM_APP + 7, 0, (LPARAM)new std::wstring(L"Status: Failed to send config."));
        return false;
    }

    running_ = true;
    recvThread_ = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)RecvThreadProc, this, 0, NULL);
    // The status is now set from main.cpp after the call to Connect.
    // if (targetHwnd_) PostMessageW(targetHwnd_, WM_APP + 7, 0, (LPARAM)new std::wstring(L"Status: Listening..."));
    return true;
}

bool GeminiLiveClient::SendWsTextFrame(const std::string& payload) {
    if (sock_ == INVALID_SOCKET || !running_ || !tls_established_) return false;

    std::vector<char> frame;
    frame.push_back((char)0x81); // FIN + Text frame

    size_t payload_len = payload.length();
    if (payload_len <= 125) {
        frame.push_back((char)(payload_len | 0x80)); // Masked
    } else if (payload_len <= 65535) {
        frame.push_back((char)(126 | 0x80)); // Masked
        frame.push_back((char)((payload_len >> 8) & 0xFF));
        frame.push_back((char)(payload_len & 0xFF));
    } else {
        frame.push_back((char)(127 | 0x80)); // Masked
        for (int i = 7; i >= 0; --i) {
            frame.push_back((char)(((uint64_t)payload_len >> (i * 8)) & 0xFF));
        }
    }

    // Masking key
    char mask[4];
    HCRYPTPROV hProv;
    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(hProv, sizeof(mask), (BYTE*)mask);
        CryptReleaseContext(hProv, 0);
    }
    frame.insert(frame.end(), mask, mask + 4);

    size_t payload_start = frame.size();
    frame.insert(frame.end(), payload.begin(), payload.end());
    for (size_t i = 0; i < payload_len; ++i) {
        frame[payload_start + i] ^= mask[i % 4];
    }

    return SendTls(frame.data(), (int)frame.size()) > 0;
}

void GeminiLiveClient::Close() {
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

    if (hCtxt_.dwLower != 0 || hCtxt_.dwUpper != 0) {
        DeleteSecurityContext(&hCtxt_);
        hCtxt_ = {};
    }
    if (hCred_.dwLower != 0 || hCred_.dwUpper != 0) {
        FreeCredentialsHandle(&hCred_);
        hCred_ = {};
    }
    tls_established_ = false;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    dec_buffer_.clear();
}

int GeminiLiveClient::SendTls(const char* data, int len) {
    if (!tls_established_) return -1;

    std::vector<char> message(streamSizes_.cbHeader + len + streamSizes_.cbTrailer);
    memcpy(message.data() + streamSizes_.cbHeader, data, len);

    SecBuffer bufs[4];
    bufs[0].pvBuffer = message.data();
    bufs[0].cbBuffer = streamSizes_.cbHeader;
    bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
    bufs[1].pvBuffer = message.data() + streamSizes_.cbHeader;
    bufs[1].cbBuffer = len;
    bufs[1].BufferType = SECBUFFER_DATA;
    bufs[2].pvBuffer = message.data() + streamSizes_.cbHeader + len;
    bufs[2].cbBuffer = streamSizes_.cbTrailer;
    bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
    bufs[3].BufferType = SECBUFFER_EMPTY;

    SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
    if (EncryptMessage(&hCtxt_, 0, &desc, 0) != SEC_E_OK) return -1;

    int total_len = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
    if (send(sock_, message.data(), total_len, 0) == SOCKET_ERROR) return -1;
    
    return len;
}

int GeminiLiveClient::RecvTls(char* buffer, int len) {
    if (!tls_established_) return -1;
    
    if (!dec_buffer_.empty()) {
        size_t to_copy = std::min((size_t)len, dec_buffer_.size());
        memcpy(buffer, dec_buffer_.data(), to_copy);
        dec_buffer_.erase(dec_buffer_.begin(), dec_buffer_.begin() + to_copy);
        return (int)to_copy;
    }

    char temp_buf[8192];
    int bytes_read = recv(sock_, temp_buf, sizeof(temp_buf), 0);
    if (bytes_read <= 0) return bytes_read;

    dec_buffer_.assign(temp_buf, temp_buf + bytes_read);

    while (true) {
        SecBuffer bufs[4] = {};
        bufs[0].pvBuffer = dec_buffer_.data();
        bufs[0].cbBuffer = (unsigned long)dec_buffer_.size();
        bufs[0].BufferType = SECBUFFER_DATA;
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
        SECURITY_STATUS status = DecryptMessage(&hCtxt_, &desc, 0, NULL);

        if (status == SEC_E_INCOMPLETE_MESSAGE) break;
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) return -1;
        // Process decrypted data and handle SECBUFFER_EXTRA...
        // For simplicity, we'll assume full messages are decrypted and just use the buffer.
        // A full implementation is much more complex.
        return RecvTls(buffer, len); // Recursive call to serve from buffer
    }

    return 0; // Incomplete message, need more data from socket.
}

unsigned int GeminiLiveClient::RecvThreadProc(void* param) {
    static_cast<GeminiLiveClient*>(param)->RunRecvLoop();
    return 0;
}

bool GeminiLiveClient::SendAudio(const char* data, size_t len) {
    if (len == 0) return false;
    if (sock_ == INVALID_SOCKET || !running_ || !tls_established_) return false;

    std::string b64_audio = Base64Encode(data, len);
    // Match the format from the JS reference implementation
    std::string json_payload = "{\"media\": {\"data\": \"" + b64_audio + "\", \"mimeType\": \"audio/pcm;rate=16000\"}}";
    return SendWsTextFrame(json_payload);
}

bool GeminiLiveClient::SendTextPrompt(const std::string& prompt) {
    if (prompt.empty()) return false;
    if (sock_ == INVALID_SOCKET || !running_ || !tls_established_) return false;

    // The API expects text prompts to be part of the 'contents' array.
    std::string json_payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + json_escape(prompt) + "\"}]}]}";
    return SendWsTextFrame(json_payload);
}

void GeminiLiveClient::RunRecvLoop() {
    std::vector<char> buffer(8192);
    while (running_) {
        int bytes = RecvTls(buffer.data(), (int)buffer.size() - 1);
        if (bytes <= 0) {
            running_ = false;
            if (targetHwnd_) PostMessageW(targetHwnd_, WM_APP + 7, 0, (LPARAM)new std::wstring(L"Status: Disconnected."));
            break;
        }

        if ((buffer[0] & 0x0F) == 0x01) { // Text frame
            size_t len = (buffer[1] & 0x7F);
            size_t offset = 2;
            if (len == 126) {
                if (bytes < 4) continue;
                len = (static_cast<unsigned char>(buffer[2]) << 8) | static_cast<unsigned char>(buffer[3]);
                offset = 4;
            } else if (len == 127) {
                // Large frames not supported in this simple client
                continue;
            }

            if ((size_t)bytes < offset + len) continue; // Incomplete frame

            std::string payload(buffer.data() + offset, len);
            
            // --- User transcript ---
            size_t user_pos = payload.find("\"inputTranscription\"");
            if (user_pos != std::string::npos) {
                size_t text_pos = payload.find("\"text\":", user_pos);
                if (text_pos != std::string::npos) {
                    size_t start_quote = payload.find('"', text_pos + 7);
                    size_t end_quote = payload.find('"', start_quote + 1);
                    if (start_quote != std::string::npos && end_quote != std::string::npos) {
                        std::string transcript = payload.substr(start_quote + 1, end_quote - start_quote - 1);
                        bool is_final = (payload.find("\"isFinal\": true", user_pos) != std::string::npos);
                        
                        if (!transcript.empty() && targetHwnd_) {
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, transcript.c_str(), -1, NULL, 0);
                            if (wlen > 0) {
                                wchar_t* wbuf = new wchar_t[wlen];
                                MultiByteToWideChar(CP_UTF8, 0, transcript.c_str(), -1, wbuf, wlen);
                                PostMessageW(targetHwnd_, WM_APP_USER_TRANSCRIPT, (WPARAM)is_final, (LPARAM)wbuf);
                            }
                        }
                    }
                }
            }

            // --- AI transcript and audio ---
            size_t agent_pos = payload.find("\"modelTurn\"");
            if (agent_pos != std::string::npos) {
                size_t text_part_pos = payload.find("\"text\":", agent_pos);
                if (text_part_pos != std::string::npos) {
                    size_t start_quote = payload.find('"', text_part_pos + 7);
                    size_t end_quote = payload.find('"', start_quote + 1);
                    if (start_quote != std::string::npos && end_quote != std::string::npos) {
                        std::string transcript = payload.substr(start_quote + 1, end_quote - start_quote - 1);
                        if (!transcript.empty() && targetHwnd_) {
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, transcript.c_str(), -1, NULL, 0);
                            if (wlen > 0) {
                                wchar_t* wbuf = new wchar_t[wlen];
                                MultiByteToWideChar(CP_UTF8, 0, transcript.c_str(), -1, wbuf, wlen);
                                PostMessageW(targetHwnd_, WM_APP_AGENT_TRANSCRIPT, 0, (LPARAM)wbuf);
                            }
                        }
                    }
                }

                size_t audio_part_pos = payload.find("\"inlineData\"", agent_pos);
                if (audio_part_pos != std::string::npos) {
                    size_t data_pos = payload.find("\"data\":", audio_part_pos);
                    if (data_pos != std::string::npos) {
                        size_t start_quote = payload.find('"', data_pos + 7);
                        size_t end_quote = payload.find('"', start_quote + 1);
                        if (start_quote != std::string::npos && end_quote != std::string::npos) {
                            std::string b64_audio = payload.substr(start_quote + 1, end_quote - start_quote - 1);
                            if (!b64_audio.empty() && audioPlayer_) {
                                std::vector<char> pcm = Base64Decode(b64_audio);
                                if (!pcm.empty()) audioPlayer_->Play(pcm.data(), pcm.size());
                            }
                        }
                    }
                }
            }
        }
    }
}