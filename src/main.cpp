#define UNICODE
#define _UNICODE

// Win32/Network headers - MUST be in this order
#include <winsock2.h>
#include <windows.h>
#include <gdiplus.h>
#include <richedit.h>

// Standard C++ Library headers
#include <string>
#include <thread>
#include <iostream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <fstream>

// Project-specific headers
#include "audio_capture.h"
#include "audio_playback.h"
#include "config.h" // For GEMINI_API_KEY
#include "gemini_live_client.h"
#include "app_messages.h"

using namespace Gdiplus;

// Global Variables
const wchar_t CLASS_NAME[] = L"TaskManOverlay";
const wchar_t WINDOW_TITLE[] = L"Task Manager Service"; // Disguised Title (UI must still be visible and labeled)
HWND hGlobalWnd = NULL;
bool isStealth = false; // False = Interactive, True = Click-through/Hidden
// Initial overlay status text (wide string)
std::wstring currentText = L"Status: Ready. Press F2 to toggle Stealth Mode.";
static AudioCapture g_audioCapture;
static AudioPlayback g_audioPlayback;
static GeminiLiveClient g_liveClient;

// Child controls
HWND g_hRecordBtn = NULL;
HWND g_hSendBtn = NULL;
HWND g_hPromptEdit = NULL;
HWND g_hConvHist = NULL;
bool g_isListening = false;

// Live transcript state
std::wstring g_currentUserTranscript;
std::wstring g_currentAgentTranscript;
bool g_isLiveMessageActive = false;
bool g_isLiveMessageUser = false;


// Simple thread-safe logging to a file in the repo root: overlay.log
std::mutex g_logMutex;
static std::ofstream g_logFile;

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
    if (!g_hConvHist) return;

    // Get current text length to append at the end
    long textLen = GetWindowTextLengthW(g_hConvHist);
    SendMessageW(g_hConvHist, EM_SETSEL, (WPARAM)textLen, (LPARAM)textLen);
    
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;

    // Choose color by prefix, similar to GDI+ version
    if (line.rfind(L"Agent:", 0) == 0) {
        cf.crTextColor = RGB(173, 216, 230); // light blue
    } else if (line.rfind(L"You:", 0) == 0) {
        cf.crTextColor = RGB(255, 215, 0); // gold
    } else { // system
        cf.crTextColor = RGB(160, 160, 160); // dim gray
    }
    SendMessageW(g_hConvHist, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    // Append the new line. It will inherit the format of the insertion point.
    std::wstring lineWithNewline = line + L"\r\n";
    SendMessageW(g_hConvHist, EM_REPLACESEL, FALSE, (LPARAM)lineWithNewline.c_str());

    // The RichEdit control has a text limit. Let's trim the top if it gets too long.
    long lineCount = SendMessage(g_hConvHist, EM_GETLINECOUNT, 0, 0);
    if (lineCount > 201) {
        long secondLineStart = SendMessage(g_hConvHist, EM_LINEINDEX, 1, 0);
        SendMessage(g_hConvHist, EM_SETSEL, 0, secondLineStart);
        SendMessage(g_hConvHist, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
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

void UpdateLiveTranscript(const std::wstring& text, bool isUser, bool isFinal) {
    if (!g_hConvHist) return;

    // If the speaker changes, the previous active message is now implicitly final.
    // We'll mark it as inactive so a new line is appended.
    if (g_isLiveMessageActive && isUser != g_isLiveMessageUser) {
        g_isLiveMessageActive = false;
    }

    long start_sel;
    if (g_isLiveMessageActive) {
        // An active message exists, so we'll replace it.
        // Find the start of the last line in the RichEdit control.
        long line_count = SendMessage(g_hConvHist, EM_GETLINECOUNT, 0, 0);
        start_sel = (line_count > 0) ? SendMessage(g_hConvHist, EM_LINEINDEX, line_count - 1, 0) : 0;
    } else {
        // No active message, so append a new line.
        long end_sel = GetWindowTextLengthW(g_hConvHist);
        SendMessageW(g_hConvHist, EM_SETSEL, end_sel, end_sel);
        // Add a newline if we're not at the beginning of the text.
        if (end_sel > 0) {
            SendMessageW(g_hConvHist, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
        }
        start_sel = GetWindowTextLengthW(g_hConvHist);
    }

    // Select the area to be replaced (from the start of the last line to the end of the text).
    SendMessageW(g_hConvHist, EM_SETSEL, start_sel, -1);

    // Set the text color based on the speaker.
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    if (isUser) {
        cf.crTextColor = RGB(255, 215, 0); // Gold for user
    } else { // Agent
        cf.crTextColor = RGB(173, 216, 230); // Light blue for agent
    }
    SendMessageW(g_hConvHist, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    // Prepend speaker and append "..." if the message is not final.
    std::wstring prefix = isUser ? L"You: " : L"Agent: ";
    std::wstring displayText = prefix + text + (isFinal ? L"" : L"...");
    SendMessageW(g_hConvHist, EM_REPLACESEL, FALSE, (LPARAM)displayText.c_str());

    // Update the state for the next call.
    g_isLiveMessageActive = !isFinal;
    g_isLiveMessageUser = isUser;

    SendMessageW(g_hConvHist, WM_VSCROLL, SB_BOTTOM, 0);
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
    if (g_hConvHist) EnableWindow(g_hConvHist, !isStealth);

    // Force a repaint to update text
    InvalidateRect(hwnd, NULL, TRUE);
}

void SubmitPromptToLLM(const std::string& prompt, HWND hwnd) {
    if (prompt.empty()) {
        LogEvent("SubmitPromptToLLM: called with empty prompt.");
        return;
    }

    // Log and add to conversation
    LogEvent(std::string("UI: sending prompt: ") + prompt);
    int qwlen = MultiByteToWideChar(CP_UTF8, 0, prompt.c_str(), -1, NULL, 0);
    if (qwlen > 0) {
        std::wstring wq; wq.resize(qwlen);
        MultiByteToWideChar(CP_UTF8, 0, prompt.c_str(), -1, &wq[0], qwlen);
        if (!wq.empty() && wq.back() == L'\0') wq.resize(wq.size() - 1);
        AddConversation(std::wstring(L"You: ") + wq);
    }

    currentText = L"Status: Thinking...";
    InvalidateRect(hwnd, NULL, TRUE);

    // Send the text prompt over the live WebSocket connection if it's active.
    if (g_isListening) {
        g_liveClient.SendTextPrompt(prompt);
        currentText = L"Status: Listening..."; // Return to listening status
    } else {
        // If not listening, we can't send the prompt.
        AddConversation(L"[system] Error: Must be listening to send a text prompt.");
        currentText = L"Status: Ready. Press F3 to listen.";
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

void ToggleListening(HWND hwnd) {
    if (g_isListening) {
        // Stop listening
        LogEvent("Stopping listener...");
        g_audioCapture.Stop();
        g_liveClient.Close();
        g_audioPlayback.Stop();
        g_isListening = false;
        if (g_hRecordBtn) SetWindowTextW(g_hRecordBtn, L"Listen");
        
        // Finalize any active message in the transcript view
        if (g_isLiveMessageActive) {
            if (g_isLiveMessageUser) {
                UpdateLiveTranscript(g_currentUserTranscript, true, true);
            } else {
                UpdateLiveTranscript(g_currentAgentTranscript, false, true);
            }
            g_isLiveMessageActive = false;
        }
        g_currentUserTranscript.clear();
        g_currentAgentTranscript.clear();
    } else {
        // Start listening
        LogEvent("Starting listener...");
        if (!g_audioPlayback.Start()) {
            LogEvent("Failed to start audio playback.");
            AddConversation(L"[system] Error: Could not start audio playback.");
            return;
        }
        g_liveClient.SetAudioPlayer(&g_audioPlayback);
        g_liveClient.SetTargetWindow(hwnd);
        g_liveClient.Connect(GEMINI_API_KEY); // This is async and posts status

        g_audioCapture.SetBufferCallback([](const char* data, size_t len){ g_liveClient.SendAudio(data, len); });
        g_audioCapture.Start("capture.wav"); // File is for backup, streaming is primary
        g_isListening = true;
        if (g_hRecordBtn) SetWindowTextW(g_hRecordBtn, L"Stop");
        AddConversation(std::wstring(L"[system] Listener started..."));
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        // Register Hotkeys (also registered after CreateWindow in WinMain)
        RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_F2); // toggle stealth
        RegisterHotKey(hwnd, 2, MOD_NOREPEAT, VK_F3); // start/stop listening
    // Create child controls: prompt edit, send button, and record toggle
        RECT crect; GetClientRect(hwnd, &crect);
        int edX = 10, edY = 40, edW = 380, edH = 24;
        int sendX = edX + edW + 10, sendW = 70;
        int recX = sendX + sendW + 10, recW = 90;
        g_hPromptEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
            edX, edY, edW, edH, hwnd, (HMENU)1001, NULL, NULL);
        g_hSendBtn = CreateWindowExW(0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            sendX, edY, sendW, edH, hwnd, (HMENU)1002, NULL, NULL);
        g_hRecordBtn = CreateWindowExW(0, L"BUTTON", L"Listen", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            recX, edY, recW, edH, hwnd, (HMENU)1003, NULL, NULL);
        // Initially enable/disable based on stealth
        if (isStealth) {
            EnableWindow(g_hPromptEdit, FALSE);
            EnableWindow(g_hSendBtn, FALSE);
            EnableWindow(g_hRecordBtn, FALSE);
        }
        // Create RichEdit control for conversation history
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        int convY = edY + edH + 10;
        g_hConvHist = CreateWindowExW(
            0, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
            10, convY, rcClient.right - 20, rcClient.bottom - convY - 10,
            hwnd, (HMENU)1004, NULL, NULL);
        // Set dark background and default text color
        if (g_hConvHist) SendMessageW(g_hConvHist, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(20, 20, 20));
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
        LogEvent("WM_DESTROY: exiting");
        if (g_isListening) ToggleListening(hwnd); // Cleanly stop listening
        if (g_logFile.is_open()) g_logFile.close();
        PostQuitMessage(0);
        return 0;

    case WM_HOTKEY:
        if (wParam == 1) { // F2 pressed
            LogEvent("Hotkey F2 pressed: ToggleStealth");
            ToggleStealth(hwnd);
        } else if (wParam == 2) { // F3 start/stop listening
            LogEvent("Hotkey F3 pressed: Toggle Listening");
            ToggleListening(hwnd);
        }
        return 0;

    case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == 1003 && code == BN_CLICKED) { // Record button
                LogEvent("UI: Listen/Stop button clicked");
                ToggleListening(hwnd);

            } else if (id == 1002 && code == BN_CLICKED) { // Send button
                LogEvent("UI: Send button clicked");
                if (!g_hPromptEdit) return 0;

                int len = GetWindowTextLengthW(g_hPromptEdit);
                if (len == 0) return 0;

                std::wstring wbuf; wbuf.resize(len + 1);
                GetWindowTextW(g_hPromptEdit, &wbuf[0], len + 1);
                wbuf.resize(len);

                // convert to UTF-8
                int utf8len = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, NULL, 0, NULL, NULL);
                std::string q;
                if (utf8len > 0) {
                    q.resize(utf8len);
                    WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, &q[0], utf8len, NULL, NULL);
                    if (!q.empty() && q.back() == '\0') q.pop_back();
                }

                SubmitPromptToLLM(q, hwnd);

                if (!q.empty()) {
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
            // Also resize conversation history
            int convY = edY + edH + 10;
            int convH = std::max(10, (int)crect.bottom - convY - 10);
            int convW = std::max(100, width - 20);
            if (g_hConvHist) MoveWindow(g_hConvHist, 10, convY, convW, convH, TRUE);
            InvalidateRect(hwnd, NULL, TRUE);
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

            // Copy from memory to screen
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, hdcMem, 0, 0, SRCCOPY);

            // Cleanup
            SelectObject(hdcMem, hOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hwnd, &ps);
        }
        return 0;
    
    case WM_APP_USER_TRANSCRIPT: { // User transcript update
        wchar_t* txt = reinterpret_cast<wchar_t*>(lParam);
        bool is_final = (bool)wParam;
        if (txt) {
            // The API sends the full segment for user speech, so we replace our copy.
            g_currentUserTranscript = std::wstring(txt);
            UpdateLiveTranscript(g_currentUserTranscript, true, is_final);

            if (is_final) {
                g_currentUserTranscript.clear();
                currentText = L"Status: Listening...";
            } else {
                currentText = L"Status: User speaking...";
            }
            InvalidateRect(hwnd, NULL, TRUE);
            delete[] txt;
        }
        return 0;
    }
    case WM_APP_AGENT_TRANSCRIPT: { // Agent transcript update
        wchar_t* txt = reinterpret_cast<wchar_t*>(lParam);
        if (txt) {
            // The API sends chunks for the agent's speech, so we append.
            g_currentAgentTranscript += txt;
            UpdateLiveTranscript(g_currentAgentTranscript, false, false);
            currentText = L"Status: Agent responding...";
            InvalidateRect(hwnd, NULL, TRUE);
            delete[] txt;
        }
        return 0;
    }
    case WM_APP_CONNECTION_STATUS: { // Generic status update from a worker thread
        std::wstring* pStatus = reinterpret_cast<std::wstring*>(lParam);
        if (pStatus) {
            currentText = *pStatus;
            InvalidateRect(hwnd, NULL, TRUE);
            delete pStatus;

            // If connection failed, update listening state
            if (g_isListening && currentText.find(L"failed") != std::wstring::npos) {
                 LogEvent("Connection failed, stopping listener UI.");
                g_isListening = false;
                if (g_hRecordBtn) SetWindowTextW(g_hRecordBtn, L"Listen");
                AddConversation(L"[system] " + currentText);
            }
        }
        return 0;
    }
    case WM_APP_INTERRUPTED: {
        LogEvent("AI response interrupted by user.");
        if (g_audioPlayback.IsRunning()) {
            g_audioPlayback.Stop();
            g_audioPlayback.Start();
        }
        if (!g_currentAgentTranscript.empty()) {
            AddConversation(L"Agent: " + g_currentAgentTranscript + L" [interrupted]");
            g_currentAgentTranscript.clear();
        }
        return 0;
    }

    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// Entry Point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
    // Load Rich Edit control library
    LoadLibraryW(L"Msftedit.dll");

    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Status gstatus = GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    if (gstatus != Ok) {
        MessageBoxW(NULL, L"Failed to initialize Gdiplus", L"Error", MB_ICONERROR);
        return 1;
    }

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
