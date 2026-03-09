// Minimal WinHTTP WebSocket client helper for streaming audio files to a STT WebSocket
#pragma once
#include <windows.h>
#include <string>

// Stream the given local file (binary) to a WebSocket STT endpoint and post
// interim transcript messages to the provided HWND via WM_APP+1 (LPARAM is a
// heap-allocated wchar_t* that the caller window will free).
// Parameters:
//  - host: hostname (e.g. L"127.0.0.1")
//  - port: port number (e.g. 8000)
//  - path: websocket path (e.g. L"/ws/stt")
//  - filepath: local path to WAV file to stream
//  - hwnd: window to PostMessage transcripts to (may be NULL)
// Returns true on success.
bool stream_file_to_stt_ws(const std::wstring& host, unsigned short port, const std::wstring& path, const std::string& filepath, HWND hwnd);
