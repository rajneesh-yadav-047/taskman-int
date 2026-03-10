#pragma once
#include <windows.h>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    bool Start();
    void Stop();
    void Play(const char* data, size_t size);
    bool IsRunning() const { return running_.load(); }

private:
    void PlaybackThread();
    static void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);

    HWAVEOUT hWaveOut_ = NULL;
    std::atomic<bool> running_{false};
    std::thread playbackThread_;

    // Data queue and synchronization primitives
    std::vector<char> playbackQueue_;
    std::mutex playbackMutex_;
    std::condition_variable playbackCv_;
};