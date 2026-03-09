# C++ Architecture (native client)

This document describes a C++-centric architecture for the transparent assistant.

Components (C++ native):

- Client (native Win32 C++):
  - Overlay / UI: Win32 window that uses GDI+ for rendering captions and suggestions. Always visible and clearly labelled.
  - Audio capture: WASAPI for microphone capture; optional loopback for system audio.
  - Network: libcurl for HTTP calls to cloud STT/LLM or to a local helper service.
  - Persistence: SQLite or simple encrypted files for resume and settings.

- Optional local helper (Python or C++):
  - If you plan to use cloud-only STT/LLM, the client can directly call REST APIs.
  - For complex streaming scenarios, a local helper service (WebSocket/STT adapter) can be used.

Libraries & Tools

- C++17
- CMake for build
- libcurl for HTTP
- nlohmann/json (optional) for JSON handling
- WASAPI (Windows SDK) for audio capture
- GDI+ for rendering
- SQLite (optional) for local storage

Notes

- Keep UI obvious about recording state.
- Users must manually request screenshot/OCR; do not capture automatically without clear consent.
- For low-latency STT, use streaming endpoints from cloud providers or a local model wrapped by a small helper service.

Milestones (C++)

1. Compile native overlay (this scaffold).  
2. Add WASAPI capture and save small audio chunks to disk or stream to a helper.  
3. Wire libcurl to POST audio chunks to STT endpoint and receive transcriptions.  
4. Build prompt composer and call LLM for suggestions.  
5. Add resume upload and local indexing to augment prompts.

