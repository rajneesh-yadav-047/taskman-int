# Architecture Overview

This document describes the high-level architecture for the ethical, transparent real-time assistant (live captions, resume-aware suggestions, screenshot-on-demand).

Components

- Client (Electron app)
  - Overlay UI: transparent/always-on-top BrowserWindow showing live captions and suggestion panel (clearly labeled).
  - Audio capture: getUserMedia for microphone; native modules can be added for WASAPI loopback.
  - Screenshot capture: user-initiated via call to Desktop Duplication API / navigator.mediaDevices.getDisplayMedia.
  - Resume upload & settings UI.
  - Auth flow (OAuth) via popup to backend.

- Backend (FastAPI)
  - Prompt builder and LLM adapter service.
  - STT adapter (optional): proxy to cloud STT or local model.
  - Vision adapter: OCR preprocessor and image cropper.
  - WebSocket streaming endpoint for low-latency communication.

- Services
  - STT (Google Cloud / Whisper local)
  - LLM (Gemini / OpenAI / other)
  - Vision / OCR (Google Vision / Tesseract)

Data Flow

1. Client captures microphone audio and streams short chunks to STT (locally or via backend).
2. Backend returns interim transcripts to client; client displays live captions.
3. User presses "Request Suggestion"; client sends recent context + resume + optional screenshot to backend.
4. Backend composes a prompt and queries the LLM, returning suggested responses.
5. Client displays suggestions; user chooses to use them manually.

Privacy & Consent

- Client always shows explicit recording indicator when capturing audio.
- Screenshots are taken only on explicit user action.
- All data storage is local by default; uploads to cloud are opt-in and encrypted.

Milestones

1. Add Electron skeleton with visible overlay UI and audio permission flow.
2. Add FastAPI backend skeleton with a test LLM endpoint.
3. Implement streaming STT (using local Whisper or cloud STT) and show live captions.
4. Add prompt builder + LLM integration and resume upload.
5. Add vision/OCR and screenshot integration.

This repo scaffold is intentionally minimal so we can iterate file-by-file.