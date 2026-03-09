#include "audio_capture.h"
#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <functional>
#include <mutex>

#pragma comment(lib, "winmm.lib")

struct WaveHeader {
	char riff[4]; // "RIFF"
	uint32_t overall_size;
	char wave[4]; // "WAVE"
	char fmt_chunk_marker[4]; // "fmt "
	uint32_t length_of_fmt;
	uint16_t format_type;
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t byterate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	char data_chunk_header[4]; // "data"
	uint32_t data_size;
};

AudioCapture::AudioCapture() {}
AudioCapture::~AudioCapture() { Stop(); }

// Simple blocking capture: writes to memory buffer and then dumps WAV on Stop
bool AudioCapture::Start(const std::string& outWavPath) {
	if (running_) return false;
	outPath_ = outWavPath;
	pcmData_.clear();

	// We'll perform a simple capture using waveInOpen in a dedicated thread.
	running_ = true;

	std::thread([this]() {
		HWAVEIN hWaveIn = NULL;
		WAVEFORMATEX wfx = {0};
		wfx.wFormatTag = WAVE_FORMAT_PCM;
		wfx.nChannels = 1; // mono
		wfx.nSamplesPerSec = 16000; // 16kHz
		wfx.wBitsPerSample = 16; // 16-bit
		wfx.nBlockAlign = (wfx.wBitsPerSample / 8) * wfx.nChannels;
		wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

		if (waveInOpen(&hWaveIn, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
			running_ = false;
			return;
		}

		const int BUFS = 4;
		const int BUF_SIZE = (int)wfx.nAvgBytesPerSec / 2; // half-second buffers
		std::vector<WAVEHDR> headers(BUFS);
		std::vector<std::vector<char>> bufs(BUFS, std::vector<char>(BUF_SIZE));

		for (int i = 0; i < BUFS; ++i) {
			headers[i].lpData = bufs[i].data();
			headers[i].dwBufferLength = BUF_SIZE;
			headers[i].dwFlags = 0;
			waveInPrepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
			waveInAddBuffer(hWaveIn, &headers[i], sizeof(WAVEHDR));
		}

		waveInStart(hWaveIn);

		while (running_) {
			for (int i = 0; i < BUFS && running_; ++i) {
				// wait for buffer to be done
				while (!(headers[i].dwFlags & WHDR_DONE) && running_) {
					Sleep(10);
				}
				if (!running_) break;
				// append captured samples
				pcmData_.insert(pcmData_.end(), headers[i].lpData, headers[i].lpData + headers[i].dwBytesRecorded);
				// invoke buffer callback if registered
				{
					std::lock_guard<std::mutex> lk(cbMutex_);
					if (bufferCallback_) bufferCallback_(headers[i].lpData, headers[i].dwBytesRecorded);
				}
				// requeue buffer
				headers[i].dwFlags = 0;
				headers[i].dwBytesRecorded = 0;
				waveInAddBuffer(hWaveIn, &headers[i], sizeof(WAVEHDR));
			}
		}

		waveInStop(hWaveIn);
		waveInReset(hWaveIn);
		for (int i = 0; i < BUFS; ++i) waveInUnprepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
		waveInClose(hWaveIn);

		// write WAV file
		std::ofstream ofs(outPath_, std::ios::binary);
		if (!ofs) return;

		WaveHeader wh;
		memcpy(wh.riff, "RIFF", 4);
		memcpy(wh.wave, "WAVE", 4);
		memcpy(wh.fmt_chunk_marker, "fmt ", 4);
		wh.length_of_fmt = 16;
		wh.format_type = 1; // PCM
		wh.channels = 1;
		wh.sample_rate = 16000;
		wh.bits_per_sample = 16;
		wh.byterate = wh.sample_rate * wh.channels * (wh.bits_per_sample/8);
		wh.block_align = (wh.bits_per_sample/8) * wh.channels;
		memcpy(wh.data_chunk_header, "data", 4);
		wh.data_size = (uint32_t)pcmData_.size();
		wh.overall_size = 36 + wh.data_size;

		ofs.write((char*)&wh, sizeof(WaveHeader));
		if (!pcmData_.empty()) ofs.write(pcmData_.data(), pcmData_.size());
		ofs.close();

	}).detach();

	return true;
}

void AudioCapture::Stop() {
	if (!running_) return;
	running_ = false;
	// give capture thread a moment to finish and write file
	Sleep(200);
}

void AudioCapture::SetBufferCallback(std::function<void(const char*, size_t)> cb) {
	std::lock_guard<std::mutex> lk(cbMutex_);
	bufferCallback_ = cb;
}
