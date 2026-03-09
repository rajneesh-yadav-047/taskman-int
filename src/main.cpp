#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <gdiplus.h>
#include <thread>
#include <iostream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>

#include "http_client.h"
#include "audio_capture.h"
#include "prompt_client.h"
#include "stt_ws_client.h"
#include "stt_ws_socket.h"
#include <fstream>

using namespace Gdiplus;

// Global Variables
const wchar_t CLASS_NAME[] = L"TaskManOverlay";
const wchar_t WINDOW_TITLE[] = L"Task Manager Service"; // Disguised Title (UI must still be visible and labeled)
HWND hGlobalWnd = NULL;
bool isStealth = false; // False = Interactive, True = Click-through/Hidden
// Initial overlay status text (wide string)
std::wstring currentText = L"Status: Ready. Press F2 to toggle Stealth Mode.";
static AudioCapture g_audioCapture;
static STTWebSocket g_sttSocket;
// Child controls
HWND g_hRecordBtn = NULL;
HWND g_hSendBtn = NULL;
HWND g_hPromptEdit = NULL;
bool g_isRecording = false;

// Simple thread-safe logging to a file in the repo root: overlay.log
std::mutex g_logMutex;
static std::ofstream g_logFile;

// Conversation buffer (recent user/assistant exchanges)
std::mutex g_convMutex;
std::vector<std::wstring> g_conversation;
// conversation scroll offset: 0 = bottom (auto-scroll), >0 = scrolled up
int g_convScroll = 0;

// subclass original edit proc so Enter can send
WNDPROC g_oldEditProc = NULL;

LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
        // Trigger Send button click in parent
        HWND parent = GetParent(hWnd);
        if (parent) {
            PostMessageW(parent, WM_COMMAND, MAKEWPARAM(1002, BN_CLICKED), (LPARAM)hWnd);
        }
        return 0; // swallow
    }
    return CallWindowProcW(g_oldEditProc, hWnd, uMsg, wParam, lParam);
}

void AddConversation(const std::wstring &line) {
    std::lock_guard<std::mutex> lk(g_convMutex);
    g_conversation.push_back(line);
    // Keep last 200 entries
    while (g_conversation.size() > 200) g_conversation.erase(g_conversation.begin());
    // auto-scroll to bottom when new message arrives
    g_convScroll = 0;
}

void LogEvent(const std::string &msg) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_logFile.is_open()) {
        g_logFile.open("overlay.log", std::ios::app);
    }
    // Timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " " << msg << std::endl;
    if (g_logFile.is_open()) {
        g_logFile << ss.str();
        g_logFile.flush();
    }
}

// Function to toggle "Click-Through" style
void ToggleStealth(HWND hwnd) {
    isStealth = !isStealth;
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

    if (isStealth) {
        // Add TRANSPARENT (click-through) and LAYERED
        SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT | WS_EX_LAYERED);
        // Set opacity to 20% (50/255) for stealth
        SetLayeredWindowAttributes(hwnd, 0, 50, LWA_ALPHA);
        currentText = L"Status: STEALTH MODE (Click-Through Active)";
    }
    else {
        // Remove TRANSPARENT (clickable) but keep LAYERED for opacity
        SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
        // Set opacity to 90% (230/255) for reading
        SetLayeredWindowAttributes(hwnd, 0, 230, LWA_ALPHA);
        currentText = L"Status: INTERACTIVE MODE (Draggable)";
    }

    // Enable/disable interactive controls when in stealth
    if (g_hRecordBtn) EnableWindow(g_hRecordBtn, !isStealth);
    if (g_hSendBtn) EnableWindow(g_hSendBtn, !isStealth);
    if (g_hPromptEdit) EnableWindow(g_hPromptEdit, !isStealth);

    // Force a repaint to update text
    InvalidateRect(hwnd, NULL, TRUE);
}


// Window Procedure (Event Handler)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        // Register Hotkeys (also registered after CreateWindow in WinMain)
        RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_F2); // toggle stealth
        RegisterHotKey(hwnd, 2, MOD_NOREPEAT, VK_F3); // start capture
        RegisterHotKey(hwnd, 3, MOD_NOREPEAT, VK_F4); // stop capture
        RegisterHotKey(hwnd, 4, MOD_NOREPEAT, VK_F5); // LLM suggestion
    // Create child controls: prompt edit, send button, and record toggle
        RECT crect; GetClientRect(hwnd, &crect);
        int edX = 10, edY = 40, edW = 380, edH = 24;
        int sendX = edX + edW + 10, sendW = 70;
        int recX = sendX + sendW + 10, recW = 90;
        g_hPromptEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
            edX, edY, edW, edH, hwnd, (HMENU)1001, NULL, NULL);
        g_hSendBtn = CreateWindowExW(0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            sendX, edY, sendW, edH, hwnd, (HMENU)1002, NULL, NULL);
        g_hRecordBtn = CreateWindowExW(0, L"BUTTON", L"Record", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            recX, edY, recW, edH, hwnd, (HMENU)1003, NULL, NULL);
        // Initially enable/disable based on stealth
        if (isStealth) {
            EnableWindow(g_hPromptEdit, FALSE);
            EnableWindow(g_hSendBtn, FALSE);
            EnableWindow(g_hRecordBtn, FALSE);
        }
        LogEvent("WM_CREATE: controls created, hotkeys registered");
        // Subclass edit control to capture Enter key
        if (g_hPromptEdit) {
            g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_hPromptEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
            // focus edit so Enter works immediately
            SetFocus(g_hPromptEdit);
        }
        return 0;
    }

    case WM_DESTROY:
    UnregisterHotKey(hwnd, 1);
    UnregisterHotKey(hwnd, 2);
    UnregisterHotKey(hwnd, 3);
    UnregisterHotKey(hwnd, 4);
        LogEvent("WM_DESTROY: exiting");
        if (g_logFile.is_open()) g_logFile.close();
        PostQuitMessage(0);
        return 0;

    case WM_HOTKEY:
        if (wParam == 1) { // F2 pressed
            LogEvent("Hotkey F2 pressed: ToggleStealth");
            ToggleStealth(hwnd);
        } else if (wParam == 2) { // F3 start capture
            LogEvent("Hotkey F3 pressed: Start capture");
            if (!g_audioCapture.IsRunning()) {
                // register live buffer callback to stream to STT in real-time
                g_sttSocket.SetTargetWindow(hwnd);
                if (!g_sttSocket.Connect(std::wstring(L"127.0.0.1"), 8001, std::wstring(L"/ws/stt"))) {
                    // fallback: still record to file
                }
                g_audioCapture.SetBufferCallback([](const char* data, size_t len){
                    // best-effort send; ignore return
                    g_sttSocket.SendBinary(data, len);
                });
                g_audioCapture.Start("capture.wav");
                currentText = L"Status: Recording audio to capture.wav";
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } else if (wParam == 3) { // F4 stop capture
            LogEvent("Hotkey F4 pressed: Stop capture");
            if (g_audioCapture.IsRunning()) {
                g_audioCapture.Stop();
                // stop streaming and close socket
                g_sttSocket.Close();
                currentText = L"Status: Audio capture saved to capture.wav";
                InvalidateRect(hwnd, NULL, TRUE);
                // After stopping capture, attempt a best-effort one-shot file upload via STT socket
                std::thread([](HWND w){
                    try {
                        STTWebSocket tmp;
                        if (tmp.Connect(std::wstring(L"127.0.0.1"), 8001, std::wstring(L"/ws/stt"))) {
                            tmp.SetTargetWindow(w);
                            // read file and send in chunks
                            std::ifstream ifs("capture.wav", std::ios::binary);
                            if (ifs) {
                                const size_t CHUNK = 8192;
                                std::vector<char> buf(CHUNK);
                                while (ifs) {
                                    ifs.read(buf.data(), CHUNK);
                                    std::streamsize r = ifs.gcount();
                                    if (r > 0) tmp.SendBinary(buf.data(), (size_t)r);
                                }
                            }
                            tmp.Close();
                        }
                    } catch (...) {}
                }, hwnd).detach();
            }
        } else if (wParam == 4) { // F5 request suggestion
            LogEvent("Hotkey F5 pressed: Request suggestion");
            // Send a simple request with the last caption text (converted to UTF-8)
            std::thread([](){
                try {
                    // For demo: ask backend for suggestion based on static question
                    std::string q = "Provide a short helpful reply to the current question.";
                    LogEvent(std::string("Requesting suggestion for q='") + q + "'");
                    std::string resp = request_suggestion(q);

                    // Convert resp (UTF-8 JSON) into a short plain string to display.
                    // Simple extraction: look for "answer": "..." in the response JSON.
                    std::string answer;
                    size_t pos = resp.find("\"answer\"");
                    if (pos != std::string::npos) {
                        size_t colon = resp.find(':', pos);
                        if (colon != std::string::npos) {
                            size_t first_quote = resp.find('"', colon);
                            if (first_quote != std::string::npos) {
                                size_t second_quote = resp.find('"', first_quote + 1);
                                if (second_quote != std::string::npos) {
                                    answer = resp.substr(first_quote + 1, second_quote - first_quote - 1);
                                }
                            }
                        }
                    }
                    if (answer.empty()) answer = resp; // fallback: show raw response

                    // Convert UTF-8 to wide string and post to window via WM_APP+1
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, answer.c_str(), -1, NULL, 0);
                    if (wlen > 0) {
                        wchar_t* wbuf = new wchar_t[wlen];
                        MultiByteToWideChar(CP_UTF8, 0, answer.c_str(), -1, wbuf, wlen);
                        // Post pointer to window; WindowProc will take ownership and delete[] it
                        if (hGlobalWnd) {
                            PostMessageW(hGlobalWnd, WM_APP + 1, 0, reinterpret_cast<LPARAM>(wbuf));
                            LogEvent(std::string("Received suggestion: ") + answer);
                        } else {
                            delete[] wbuf;
                        }
                    }
                } catch (...) {}
            }).detach();
        }
        return 0;

    case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == 1003 && code == BN_CLICKED) { // Record button
                LogEvent("UI: Record button clicked");
                // Toggle recording
                if (!g_audioCapture.IsRunning()) {
                    LogEvent("UI: starting recording via Record button");
                    // Start recording (reuse F3 logic)
                    g_sttSocket.SetTargetWindow(hwnd);
                    if (!g_sttSocket.Connect(std::wstring(L"127.0.0.1"), 8001, std::wstring(L"/ws/stt"))) {
                        // ignore connect errors
                    }
                    g_audioCapture.SetBufferCallback([](const char* data, size_t len){ g_sttSocket.SendBinary(data, len); });
                    g_audioCapture.Start("capture.wav");
                    g_isRecording = true;
                        SetWindowTextW(g_hRecordBtn, L"Stop");
                        currentText = L"Status: Listening...";
                        AddConversation(std::wstring(L"[system] Listening"));
                    InvalidateRect(hwnd, NULL, TRUE);
                } else {
                    LogEvent("UI: stopping recording via Record button");
                    // Stop recording (reuse F4 logic)
                    g_audioCapture.Stop();
                    g_sttSocket.Close();
                    g_isRecording = false;
                    SetWindowTextW(g_hRecordBtn, L"Record");
                    currentText = L"Status: Uploading audio...";
                    AddConversation(std::wstring(L"[system] Uploading audio"));
                    InvalidateRect(hwnd, NULL, TRUE);
                    // spawn one-shot upload
                    std::thread([](HWND w){
                        try {
                            STTWebSocket tmp;
                            if (tmp.Connect(std::wstring(L"127.0.0.1"), 8001, std::wstring(L"/ws/stt"))) {
                                tmp.SetTargetWindow(w);
                                std::ifstream ifs("capture.wav", std::ios::binary);
                                if (ifs) {
                                    const size_t CHUNK = 8192;
                                    std::vector<char> buf(CHUNK);
                                    while (ifs) {
                                        ifs.read(buf.data(), CHUNK);
                                        std::streamsize r = ifs.gcount();
                                        if (r > 0) tmp.SendBinary(buf.data(), (size_t)r);
                                    }
                                }
                                tmp.Close();
                            }
                        } catch (...) {}
                    }, hwnd).detach();
                }
            } else if (id == 1002 && code == BN_CLICKED) { // Send button
                LogEvent("UI: Send button clicked");
                if (g_hPromptEdit) {
                    int len = GetWindowTextLengthW(g_hPromptEdit);
                    std::wstring wbuf; wbuf.resize(len + 1);
                    GetWindowTextW(g_hPromptEdit, &wbuf[0], len + 1);
                    // trim to actual length
                    wbuf.resize(len);
                    // convert to UTF-8
                    int utf8len = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, NULL, 0, NULL, NULL);
                    std::string q;
                    if (utf8len > 0) {
                        q.resize(utf8len);
                        WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, &q[0], utf8len, NULL, NULL);
                        // remove trailing null
                        if (!q.empty() && q.back() == '\0') q.pop_back();
                    }
                    // record into conversation and send request in background
                    LogEvent(std::string("UI: sending prompt: ") + q);
                    // convert q (utf8) back to wide for conversation
                    int qwlen = MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, NULL, 0);
                    if (qwlen > 0) {
                        std::wstring wq; wq.resize(qwlen);
                        MultiByteToWideChar(CP_UTF8, 0, q.c_str(), -1, &wq[0], qwlen);
                        // trim trailing null
                        if (!wq.empty() && wq.back() == L'\0') wq.resize(wq.size()-1);
                        AddConversation(std::wstring(L"You: ") + wq);
                    }
                    currentText = L"Status: Thinking...";
                    InvalidateRect(hwnd, NULL, TRUE);
                    std::thread([q](){
                        try {
                            std::string resp = request_suggestion(q.empty() ? std::string("") : q);
                            std::string answer;
                            size_t pos = resp.find("\"answer\"");
                            if (pos != std::string::npos) {
                                size_t colon = resp.find(':', pos);
                                if (colon != std::string::npos) {
                                    size_t first_quote = resp.find('"', colon);
                                    if (first_quote != std::string::npos) {
                                        size_t second_quote = resp.find('"', first_quote + 1);
                                        if (second_quote != std::string::npos) {
                                            answer = resp.substr(first_quote + 1, second_quote - first_quote - 1);
                                        }
                                    }
                                }
                            }
                            if (answer.empty()) answer = resp;
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, answer.c_str(), -1, NULL, 0);
                            if (wlen > 0) {
                                wchar_t* wout = new wchar_t[wlen];
                                MultiByteToWideChar(CP_UTF8, 0, answer.c_str(), -1, wout, wlen);
                                if (hGlobalWnd) {
                                    PostMessageW(hGlobalWnd, WM_APP + 1, 0, reinterpret_cast<LPARAM>(wout));
                                    LogEvent(std::string("UI: received answer: ") + answer);
                                } else {
                                    delete[] wout;
                                }
                            }
                        } catch (...) {}
                    }).detach();
                    // clear edit box after sending
                    SetWindowTextW(g_hPromptEdit, L"");
                }
            }
        }
        return 0;

    case WM_SIZE:
        {
            // Reposition child controls when window resized
            RECT crect; GetClientRect(hwnd, &crect);
            int width = crect.right - crect.left;
            int edX = 10, edY = 40, edH = 24;
            int edW = std::max(100, width - 220);
            int sendW = 70;
            int recW = 90;
            int sendX = edX + edW + 10;
            int recX = sendX + sendW + 10;
            if (g_hPromptEdit) MoveWindow(g_hPromptEdit, edX, edY, edW, edH, TRUE);
            if (g_hSendBtn) MoveWindow(g_hSendBtn, sendX, edY, sendW, edH, TRUE);
            if (g_hRecordBtn) MoveWindow(g_hRecordBtn, recX, edY, recW, edH, TRUE);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;

    case WM_MOUSEWHEEL:
        {
            int z = GET_WHEEL_DELTA_WPARAM(wParam);
            int lines = z / WHEEL_DELTA; // usually +/-1
            if (lines != 0) {
                // scroll up increases g_convScroll (shows older messages)
                // compute visible lines similar to WM_PAINT so scrolling bounds are correct
                RECT crect; GetClientRect(hwnd, &crect);
                const float startY = 70.0f;
                const float lineHeight = 18.0f;
                int visibleLines = std::max(1, (int)((crect.bottom - (int)startY) / lineHeight));
                std::lock_guard<std::mutex> lk(g_convMutex);
                int total = (int)g_conversation.size();
                int maxScroll = std::max(0, total - visibleLines);
                g_convScroll += lines;
                if (g_convScroll < 0) g_convScroll = 0;
                if (g_convScroll > maxScroll) g_convScroll = maxScroll;
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
        return 0;

    case WM_NCHITTEST:
        // Allow dragging the window by clicking anywhere (if not in stealth)
        if (!isStealth) {
            LRESULT hit = DefWindowProcW(hwnd, uMsg, wParam, lParam);
            if (hit == HTCLIENT) return HTCAPTION;
            return hit;
        }
        break;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Double buffering setup (to prevent flickering)
            HDC hdcMem = CreateCompatibleDC(hdc);
            RECT rect;
            GetClientRect(hwnd, &rect);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

            // GDI+ Graphics context
            Graphics graphics(hdcMem);
            graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

            // Clear background (Black with some transparency handled by Layered attributes)
            SolidBrush bgBrush(Color(20, 20, 20)); 
            graphics.FillRectangle(&bgBrush, 0, 0, rect.right, rect.bottom);

            // Draw Status Text
            FontFamily fontFamily(L"Segoe UI");
            Font font(&fontFamily, 12, FontStyleRegular, UnitPoint);
            SolidBrush statusBrush(Color(255, 0, 255, 0)); // Green text for status
            graphics.DrawString(currentText.c_str(), -1, &font, PointF(10, 10), &statusBrush);

            // Draw recent conversation lines (thread-safe)
            {
                std::lock_guard<std::mutex> lk(g_convMutex);
                // Smaller font for conversation
                Font convFont(&fontFamily, 10, FontStyleRegular, UnitPoint);
                float startX = 10.0f;
                float startY = 70.0f; // leave space for status and controls
                float lineHeight = 18.0f;
                // compute visible lines and starting index based on g_convScroll (0 = bottom)
                int visibleLines = std::max(1, (int)((rect.bottom - (int)startY) / lineHeight));
                int total = (int)g_conversation.size();
                int startIndex = 0;
                if (total <= visibleLines) {
                    startIndex = 0;
                } else {
                    startIndex = std::max(0, total - visibleLines - g_convScroll);
                }
                for (int i = 0; i < visibleLines && (startIndex + i) < total; ++i) {
                    const std::wstring &line = g_conversation[startIndex + i];
                    // Choose brush color by prefix (create Color first then one SolidBrush)
                    Color col(220, 192, 192, 192); // default: gray (system)
                    if (line.rfind(L"Agent:", 0) == 0) {
                        col = Color(255, 173, 216, 230); // light blue for agent
                    } else if (line.rfind(L"You:", 0) == 0) {
                        col = Color(255, 255, 215, 0); // gold for user
                    } else if (line.rfind(L"[system]", 0) == 0) {
                        col = Color(200, 160, 160, 160); // dim gray for system
                    }
                    SolidBrush brush(col);
                    PointF pt(startX, startY + i * lineHeight);
                    graphics.DrawString(line.c_str(), -1, &convFont, pt, &brush);
                }
            }

            // Copy from memory to screen
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, hdcMem, 0, 0, SRCCOPY);

            // Cleanup
            SelectObject(hdcMem, hOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hwnd, &ps);
        }
        return 0;
    
    case WM_APP + 1: {
        // lParam carries a heap-allocated wide string (wchar_t*)
        wchar_t* txt = reinterpret_cast<wchar_t*>(lParam);
        if (txt) {
            std::wstring resp(txt);
            AddConversation(std::wstring(L"Agent: ") + resp);
            currentText = L"Status: Ready";
            InvalidateRect(hwnd, NULL, TRUE);
            delete[] txt;
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// Entry Point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Status gstatus = GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    if (gstatus != Ok) {
        MessageBoxW(NULL, L"Failed to initialize Gdiplus", L"Error", MB_ICONERROR);
        return 1;
    }

    // Try pinging backend (non-blocking) — default backend port is 8001
    std::thread([](){
        try {
            std::string resp = http_post_json("http://127.0.0.1:8001/api/ping", "{}");
            (void)resp; // ignore result for now
        } catch (const std::exception& ex) {
            // ignore network errors for now
        }
    }).detach();

    // Register Window Class
    WNDCLASSW wc = { };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassW(&wc);

    // Create Window
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        WINDOW_TITLE,
        WS_POPUP,
        100, 100, 600, 400,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        GdiplusShutdown(gdiplusToken);
        return 0;
    }

    hGlobalWnd = hwnd;

    // Set initial transparency (Interactive Mode)
    SetLayeredWindowAttributes(hwnd, 0, 230, LWA_ALPHA);

    ShowWindow(hwnd, nCmdShow);

    // For testing: if a capture.wav file exists at startup, stream it to STT backend once using STTWebSocket.
    std::thread([hwnd](){
        Sleep(1000);
        WIN32_FIND_DATAA fdata;
        HANDLE h = FindFirstFileA("capture.wav", &fdata);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            try {
                STTWebSocket tmp;
                if (tmp.Connect(std::wstring(L"127.0.0.1"), 8001, std::wstring(L"/ws/stt"))) {
                    tmp.SetTargetWindow(hwnd);
                    std::ifstream ifs("capture.wav", std::ios::binary);
                    if (ifs) {
                        const size_t CHUNK = 8192;
                        std::vector<char> buf(CHUNK);
                        while (ifs) {
                            ifs.read(buf.data(), CHUNK);
                            std::streamsize r = ifs.gcount();
                            if (r > 0) tmp.SendBinary(buf.data(), (size_t)r);
                        }
                    }
                    tmp.Close();
                }
            } catch(...){}
        }
    }).detach();

    // Message Loop
    MSG msg = { };
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup GDI+
    GdiplusShutdown(gdiplusToken);
    return 0;
}
