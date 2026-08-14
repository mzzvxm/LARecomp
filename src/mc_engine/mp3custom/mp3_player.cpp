#ifndef REXGLUE_HAS_XEO3_TARGET
#include "mp3_player.h"

#include "../logging.h"

#if defined(_WIN32)

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

namespace mp3custom {
namespace {

// Everything is resampled to this on the way in, so one source voice format
// covers every file the folder can contain.
constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kChannels   = 2;
constexpr uint16_t kBitsPerSample = 16;

using PFN_XAudio2Create = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

struct Player {
    std::mutex             mutex;
    HMODULE                xaudio_dll   = nullptr;
    IXAudio2*              engine       = nullptr;
    IXAudio2MasteringVoice* master      = nullptr;
    IXAudio2SourceVoice*   voice        = nullptr;
    bool                   mf_started   = false;
    bool                   ready        = false;

    // Owns the PCM the voice is reading; kept alive until the next track.
    std::vector<uint8_t>   pcm;

    std::thread            decoder;
    // Bumped for every PlayerPlayFile(); a decode whose generation is stale
    // throws its result away instead of stomping the current track.
    std::atomic<uint32_t>  generation{0};
    std::atomic<bool>      decoding{false};
    std::atomic<bool>      playing{false};
    std::atomic<bool>      ended{false};
    float                  volume = 1.0f;
};

Player& P() {
    static Player p;
    return p;
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Full-file decode to 48 kHz / stereo / s16. MCLA plays one radio track at a
// time, so holding a whole song (~30 MB for 5 minutes) beats the bookkeeping of
// a streaming submit, and there is no underrun path to get wrong.
bool DecodeToPcm(const std::wstring& path, std::vector<uint8_t>& out) {
    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        MC_WARN("[mp3custom] MFCreateSourceReaderFromURL failed 0x{:08X}", uint32_t(hr));
        return false;
    }

    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    IMFMediaType* type = nullptr;
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kBitsPerSample);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
    }
    SafeRelease(type);
    if (FAILED(hr)) {
        MC_WARN("[mp3custom] cannot decode to PCM 0x{:08X}", uint32_t(hr));
        SafeRelease(reader);
        return false;
    }

    for (;;) {
        DWORD        flags  = 0;
        IMFSample*   sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr,
                                &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            SafeRelease(sample);
            break;
        }
        if (!sample) continue;  // gap / format change tick

        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* data = nullptr;
            DWORD len  = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &len))) {
                out.insert(out.end(), data, data + len);
                buffer->Unlock();
            }
        }
        SafeRelease(buffer);
        SafeRelease(sample);
    }

    SafeRelease(reader);
    return !out.empty();
}

}  // namespace

bool PlayerInit() {
    Player& p = P();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.ready) return true;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        MC_WARN("[mp3custom] MFStartup failed 0x{:08X}", uint32_t(hr));
        return false;
    }
    p.mf_started = true;

    // Loaded by name rather than linked, so a machine without XAudio2_9 just
    // loses custom music instead of failing to start the game.
    p.xaudio_dll = LoadLibraryW(L"XAudio2_9.dll");
    if (!p.xaudio_dll) p.xaudio_dll = LoadLibraryW(L"XAudio2_8.dll");
    if (!p.xaudio_dll) {
        MC_WARN("[mp3custom] no XAudio2 runtime found");
        return false;
    }

    auto create = reinterpret_cast<PFN_XAudio2Create>(
        GetProcAddress(p.xaudio_dll, "XAudio2Create"));
    if (!create) {
        MC_WARN("[mp3custom] XAudio2Create missing");
        return false;
    }

    hr = create(&p.engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        MC_WARN("[mp3custom] XAudio2Create failed 0x{:08X}", uint32_t(hr));
        return false;
    }

    hr = p.engine->CreateMasteringVoice(&p.master, kChannels, kSampleRate);
    if (FAILED(hr)) {
        MC_WARN("[mp3custom] CreateMasteringVoice failed 0x{:08X}", uint32_t(hr));
        SafeRelease(p.engine);
        return false;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = kChannels;
    wfx.nSamplesPerSec  = kSampleRate;
    wfx.wBitsPerSample  = kBitsPerSample;
    wfx.nBlockAlign     = kChannels * kBitsPerSample / 8;
    wfx.nAvgBytesPerSec = kSampleRate * wfx.nBlockAlign;

    hr = p.engine->CreateSourceVoice(&p.voice, &wfx);
    if (FAILED(hr)) {
        MC_WARN("[mp3custom] CreateSourceVoice failed 0x{:08X}", uint32_t(hr));
        return false;
    }

    p.ready = true;
    MC_INFO("[mp3custom] host player up (48kHz stereo)");
    return true;
}

void PlayerShutdown() {
    Player& p = P();
    if (p.decoder.joinable()) {
        p.generation.fetch_add(1);
        p.decoder.join();
    }

    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.voice) {
        p.voice->Stop(0);
        p.voice->FlushSourceBuffers();
        p.voice->DestroyVoice();
        p.voice = nullptr;
    }
    if (p.master) {
        p.master->DestroyVoice();
        p.master = nullptr;
    }
    SafeRelease(p.engine);
    if (p.xaudio_dll) {
        FreeLibrary(p.xaudio_dll);
        p.xaudio_dll = nullptr;
    }
    if (p.mf_started) {
        MFShutdown();
        p.mf_started = false;
    }
    p.ready   = false;
    p.playing = false;
}

bool PlayerPlayFile(const std::wstring& path) {
    Player& p = P();
    if (!p.ready) return false;

    PlayerStop();

    const uint32_t gen = p.generation.fetch_add(1) + 1;
    p.playing  = true;   // set before the decode so IsPlaying covers the gap
    p.ended    = false;
    p.decoding = true;

    if (p.decoder.joinable()) p.decoder.join();
    p.decoder = std::thread([gen, path]() {
        Player& pp = P();
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        std::vector<uint8_t> pcm;
        const bool ok = DecodeToPcm(path, pcm);

        if (pp.generation.load() == gen) {
            if (ok) {
                std::lock_guard<std::mutex> lock(pp.mutex);
                if (pp.voice) {
                    pp.pcm = std::move(pcm);
                    XAUDIO2_BUFFER buf{};
                    buf.Flags      = XAUDIO2_END_OF_STREAM;
                    buf.AudioBytes = static_cast<UINT32>(pp.pcm.size());
                    buf.pAudioData = pp.pcm.data();
                    pp.voice->SubmitSourceBuffer(&buf);
                    pp.voice->Start(0);
                }
            } else {
                pp.playing = false;
                pp.ended   = true;
            }
        }
        pp.decoding = false;

        CoUninitialize();
    });

    return true;
}

void PlayerStop() {
    Player& p = P();
    if (!p.ready) return;

    p.generation.fetch_add(1);  // orphan any decode still in flight
    if (p.decoder.joinable()) p.decoder.join();

    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.voice) {
        p.voice->Stop(0);
        p.voice->FlushSourceBuffers();
    }
    p.pcm.clear();
    p.pcm.shrink_to_fit();
    p.playing = false;
    p.ended   = false;
}

void PlayerPause(bool paused) {
    Player& p = P();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.ready || !p.voice) return;
    if (paused) {
        p.voice->Stop(0);
    } else if (p.playing) {
        p.voice->Start(0);
    }
}

void PlayerSetVolume(float volume) {
    Player& p = P();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.ready || !p.master) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    if (volume == p.volume) return;
    p.volume = volume;
    p.master->SetVolume(volume);
}

bool PlayerIsPlaying() {
    Player& p = P();
    if (!p.ready || !p.playing) return false;
    if (p.decoding) return true;

    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.voice) return false;
    XAUDIO2_VOICE_STATE state{};
    p.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0) {
        p.playing = false;
        p.ended   = true;
        return false;
    }
    return true;
}

bool PlayerTrackEnded() {
    Player& p = P();
    PlayerIsPlaying();  // refreshes the latch
    return p.ended.load();
}

}  // namespace mp3custom

#else  // !_WIN32

namespace mp3custom {
bool PlayerInit() { return false; }
void PlayerShutdown() {}
bool PlayerPlayFile(const std::wstring&) { return false; }
void PlayerStop() {}
void PlayerPause(bool) {}
void PlayerSetVolume(float) {}
bool PlayerIsPlaying() { return false; }
bool PlayerTrackEnded() { return false; }
}  // namespace mp3custom

#endif  // _WIN32
#endif  // REXGLUE_HAS_XEO3_TARGET
