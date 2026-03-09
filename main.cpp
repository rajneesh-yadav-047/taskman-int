#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <vector>
#include <gdiplus.h>

using namespace Gdiplus;

// Global Variables
const wchar_t CLASS_NAME[] = L"TaskManOverlay";
const wchar_t WINDOW_TITLE[] = L"Task Manager Service"; // Disguised Title
HWND hGlobalWnd = NULL;
bool isStealth = false; // False = Interactive, True = Click-through/Hidden
std::wstring currentText = L"Status: Ready. Press F2 to toggle Stealth Mode.";

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
    
    // Force a repaint to update text
    InvalidateRect(hwnd, NULL, TRUE);
}

// Window Procedure (Event Handler)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        // Register Hotkey F2 (ID: 1)
        RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_F2);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd, 1);
        PostQuitMessage(0);
        return 0;

    case WM_HOTKEY:
        if (wParam == 1) { // F2 pressed
            ToggleStealth(hwnd);
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

            // Draw Text
            FontFamily fontFamily(L"Segoe UI");
            Font font(&fontFamily, 12, FontStyleRegular, UnitPoint);
            SolidBrush textBrush(Color(255, 0, 255, 0)); // Green text

            graphics.DrawString(currentText.c_str(), -1, &font, PointF(10, 10), &textBrush);

            // Copy from memory to screen
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, hdcMem, 0, 0, SRCCOPY);

            // Cleanup
            SelectObject(hdcMem, hOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// Entry Point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Register Window Class
    WNDCLASSW wc = { };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassW(&wc);

    // Create Window
    // WS_EX_TOPMOST: Always on top
    // WS_EX_LAYERED: Allows transparency
    // WS_EX_TOOLWINDOW: Hides from Alt-Tab and Taskbar
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        WINDOW_TITLE,
        WS_POPUP, // Pop-up style (no borders)
        100, 100, 600, 400, // Position and Size
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return 0;

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
