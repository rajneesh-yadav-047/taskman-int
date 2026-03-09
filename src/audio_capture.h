// Simple audio capture using waveIn API (blocking callbacks)
#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <functional>
#include <mutex>

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Start capturing to WAV file path. Returns true on success.
    bool Start(const std::string& outWavPath);
    // Stop capturing and finalize file.
    void Stop();

    // Register a callback invoked for each captured PCM buffer.
    // Signature: void(const char* data, size_t len)
    void SetBufferCallback(std::function<void(const char*, size_t)> cb);

    bool IsRunning() const { return running_.load(); }

private:
    std::atomic<bool> running_{false};
    std::string outPath_;
    // internal buffer to accumulate PCM data before writing header
    std::vector<char> pcmData_;
    std::function<void(const char*, size_t)> bufferCallback_;
    std::mutex cbMutex_;
};
