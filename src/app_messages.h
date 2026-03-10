#pragma once
#include <windows.h> // For UINT and WM_APP

// App-specific window messages for inter-thread communication.
// These are used to post data from worker threads (like the network client)
// back to the main UI thread's message loop.
constexpr UINT WM_APP_USER_TRANSCRIPT = WM_APP + 5;
constexpr UINT WM_APP_AGENT_TRANSCRIPT = WM_APP + 6;
constexpr UINT WM_APP_CONNECTION_STATUS = WM_APP + 7;
constexpr UINT WM_APP_INTERRUPTED = WM_APP + 9;