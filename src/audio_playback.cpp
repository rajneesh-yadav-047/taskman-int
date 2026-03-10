#include "audio_playback.h"
#include <mmsystem.h>
#include <vector>
#include <iostream>

#pragma comment(lib, "winmm.lib")

namespace {
    const int NUM_PLAY_BUFFERS = 4;
    const int PLAY_BUFFER_SIZE = 4096; // Size of each buffer in bytes
}

void CALLBACK AudioPlayback::waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        // The dwInstance was set to 'this' in waveOutOpen
        AudioPlayback* player = reinterpret_cast<AudioPlayback*>(dwInstance);
        // The callback itself doesn't need to do much as the thread is polling.
    }
}

AudioPlayback::AudioPlayback() {}

AudioPlayback::~AudioPlayback() {
    Stop();
}

bool AudioPlayback::Start() {
    if (running_) return false;

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 24000; // AI audio is 24kHz
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.wBitsPerSample / 8) * wfx.nChannels;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (waveOutOpen(&hWaveOut_, WAVE_MAPPER, &wfx, (DWORD_PTR)AudioPlayback::waveOutProc, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        hWaveOut_ = NULL;
        return false;
    }

    running_ = true;
    playbackThread_ = std::thread(&AudioPlayback::PlaybackThread, this);
    return true;
}

void AudioPlayback::Stop() {
    if (!running_) return;
    running_ = false;
    playbackCv_.notify_one();
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }

    if (hWaveOut_) {
        waveOutReset(hWaveOut_);
        waveOutClose(hWaveOut_);
        hWaveOut_ = NULL;
    }
    // Clear any remaining data
    std::lock_guard<std::mutex> lock(playbackMutex_);
    playbackQueue_.clear();
}

void AudioPlayback::Play(const char* data, size_t size) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        playbackQueue_.insert(playbackQueue_.end(), data, data + size);
    }
    playbackCv_.notify_one();
}

void AudioPlayback::PlaybackThread() {
    std::vector<WAVEHDR> headers(NUM_PLAY_BUFFERS);
    std::vector<std::vector<char>> buffers(NUM_PLAY_BUFFERS, std::vector<char>(PLAY_BUFFER_SIZE));

    for (int i = 0; i < NUM_PLAY_BUFFERS; ++i) {
        headers[i].lpData = buffers[i].data();
        headers[i].dwBufferLength = PLAY_BUFFER_SIZE;
        waveOutPrepareHeader(hWaveOut_, &headers[i], sizeof(WAVEHDR));
    }

    while (running_) {
        for (int i = 0; i < NUM_PLAY_BUFFERS && running_; ++i) {
            if (headers[i].dwFlags & WHDR_INQUEUE) continue;

            std::unique_lock<std::mutex> lock(playbackMutex_);
            playbackCv_.wait(lock, [this] { return !playbackQueue_.empty() || !running_; });
            if (!running_) break;

            size_t to_play = std::min((size_t)playbackQueue_.size(), (size_t)PLAY_BUFFER_SIZE);
            memcpy(headers[i].lpData, playbackQueue_.data(), to_play);
            playbackQueue_.erase(playbackQueue_.begin(), playbackQueue_.begin() + to_play);
            headers[i].dwBufferLength = to_play;
            waveOutWrite(hWaveOut_, &headers[i], sizeof(WAVEHDR));
        }
        Sleep(10); // Yield
    }

    for (int i = 0; i < NUM_PLAY_BUFFERS; ++i) {
        if (headers[i].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hWaveOut_, &headers[i], sizeof(WAVEHDR));
        }
    }
}