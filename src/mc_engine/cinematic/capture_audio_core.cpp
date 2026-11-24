//
// The pure half of a take's sound (trailer toolkit M2c). See capture_audio_core.h.
//

#include "capture_audio_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace mc::cinematic::capture_audio {

// ---------------------------------------------------------------------------------------------------------------- the timeline

uint64_t SamplesThrough(uint64_t video_frames, double fps) {
    if (!(fps > 0.0)) return 0;  // also NaN
    // long double: n * 48000 reaches 1e11 for a day of video, and the quotient has to be right to well under a sample
    const long double exact = static_cast<long double>(video_frames) * static_cast<long double>(kRate) / static_cast<long double>(fps);
    return static_cast<uint64_t>(std::floor(exact + 0.5L));
}

uint64_t MixerFramesCovering(uint64_t samples) {
    return (samples + kMixerFrame - 1) / kMixerFrame;
}

// ---------------------------------------------------------------------------------------------------------------- the books

void Book::Begin(double fps) {
    *this = Book{};
    fps_ = fps > 0.0 ? fps : 60.0;
}

uint64_t Book::Wanted(uint64_t video_frames) const {
    const uint64_t need = MixerFramesCovering(Target(video_frames));
    // what the timeline holds or is owed already: counted frames, and the released ones on their way
    return need > released_ ? need - released_ : 0;
}

Book::Pop Book::Consume(uint64_t queued) {
    Pop p;
    p.discard = std::min(skip_, queued);
    skip_ -= p.discard;
    queued -= p.discard;
    p.use = std::min(queued, Outstanding());
    counted_ += p.use;
    return p;
}

uint64_t Book::GiveUp() {
    const uint64_t missing = Outstanding();
    counted_ += missing;
    silent_ += missing;
    skip_ += missing;
    return missing;
}

uint64_t Book::Writable(uint64_t video_frames) const {
    const uint64_t target = Target(video_frames);
    if (target <= written_) return 0;
    return std::min(target - written_, Available());
}

uint64_t Book::Shortfall(uint64_t video_frames) const {
    const uint64_t target = Target(video_frames);
    const uint64_t have = written_ + Available();
    return target > have ? target - have : 0;
}

// ---------------------------------------------------------------------------------------------------------------- the WAV file

namespace {

void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void PutTag(std::vector<uint8_t>& v, const char* tag) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(tag[i]));
}

// The speaker mask of the game's order: front left, front right, front centre, LFE, back left, back right (SPEAKER_* bits 0..5).
uint32_t ChannelMask(uint32_t channels) {
    switch (channels) {
        case 3: return 0x7;
        case 4: return 0x33;   // fl fr bl br
        case 5: return 0x37;
        case 6: return 0x3F;
        default: return 0;
    }
}

constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

}  // namespace

size_t WavHeaderSize(uint32_t channels) {
    // RIFF chunk header (12) + fmt chunk (8 + 16 or 8 + 40) + fact chunk (8 + 4) + data chunk header (8)
    return channels <= 2 ? 12 + 24 + 12 + 8 : 12 + 48 + 12 + 8;
}

std::vector<uint8_t> WavHeader(uint32_t channels, uint32_t rate, uint64_t sample_frames) {
    channels = std::max<uint32_t>(channels, 1);
    const uint64_t data64 = sample_frames * channels * 4ull;
    const uint32_t data_bytes = static_cast<uint32_t>(std::min<uint64_t>(data64, 0xFFFFFFFFull));
    const size_t header = WavHeaderSize(channels);
    const uint64_t riff64 = (header - 8) + data64;
    std::vector<uint8_t> v;
    v.reserve(header);
    PutTag(v, "RIFF");
    Put32(v, static_cast<uint32_t>(std::min<uint64_t>(riff64, 0xFFFFFFFFull)));
    PutTag(v, "WAVE");
    PutTag(v, "fmt ");
    const bool extensible = channels > 2;
    Put32(v, extensible ? 40 : 16);
    Put16(v, extensible ? kFormatExtensible : kFormatFloat);
    Put16(v, static_cast<uint16_t>(channels));
    Put32(v, rate);
    Put32(v, rate * channels * 4u);
    Put16(v, static_cast<uint16_t>(channels * 4));
    Put16(v, 32);
    if (extensible) {
        Put16(v, 22);   // cbSize
        Put16(v, 32);   // valid bits
        Put32(v, ChannelMask(channels));
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00AA00389B71}
        Put32(v, 0x00000003);
        Put16(v, 0x0000);
        Put16(v, 0x0010);
        const uint8_t tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        v.insert(v.end(), tail, tail + 8);
    }
    PutTag(v, "fact");
    Put32(v, 4);
    Put32(v, static_cast<uint32_t>(std::min<uint64_t>(sample_frames, 0xFFFFFFFFull)));
    PutTag(v, "data");
    Put32(v, data_bytes);
    return v;
}

bool WavFile::Open(const std::string& path, uint32_t channels, uint32_t rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
    channels_ = std::max<uint32_t>(channels, 1);
    rate_ = rate;
    frames_ = 0;
    failed_ = false;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        failed_ = true;
        return false;
    }
    const std::vector<uint8_t> h = WavHeader(channels_, rate_, 0);
    if (std::fwrite(h.data(), 1, h.size(), file_) != h.size()) {
        failed_ = true;
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool WavFile::Append(const float* interleaved, uint64_t sample_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) {
        failed_ = true;  // closed (the game ended during the take) or never open: the samples are lost, and Failed() says so
        return false;
    }
    if (failed_) return false;
    if (sample_frames == 0) return true;
    const uint64_t bytes = sample_frames * channels_ * 4ull;
    if ((frames_ * channels_ * 4ull) + bytes > kWavMaxDataBytes) {
        failed_ = true;  // a WAV cannot describe more; RF64 is not written
        return false;
    }
    if (std::fwrite(interleaved, sizeof(float), static_cast<size_t>(sample_frames * channels_), file_) != sample_frames * channels_) {
        failed_ = true;
        return false;
    }
    frames_ += sample_frames;
    return true;
}

bool WavFile::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    return CloseLocked();
}

bool WavFile::CloseLocked() {
    if (!file_) return !failed_;
    bool ok = !failed_;
    const std::vector<uint8_t> h = WavHeader(channels_, rate_, frames_);
    if (std::fflush(file_) != 0 || std::fseek(file_, 0, SEEK_SET) != 0 || std::fwrite(h.data(), 1, h.size(), file_) != h.size()) ok = false;
    if (std::fclose(file_) != 0) ok = false;
    file_ = nullptr;
    if (!ok) failed_ = true;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------- the take's session

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTimeoutsToDead = 3;
constexpr uint32_t kWaitSliceMs = 5;  // the longest single wait for the mixer: the loop looks at the queue again after it

}  // namespace

bool Session::Begin(MixerPort& port, const Config& config) {
    if (armed_) {
        Say(1, "a take's audio was still running: closed as it was");
        wav_.Close();
        if (port_) port_->Disarm();
        armed_ = false;
    }
    if (!(config.fps > 0.0)) return false;
    cfg_ = config;
    cfg_.channels = cfg_.channels >= 6 ? 6 : 2;
    port_ = &port;
    const int64_t thrown = port.Arm(cfg_.channels);
    if (thrown < 0) return false;  // the port said why
    {
        std::error_code ec;
        const std::filesystem::path p(cfg_.wav_path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    }
    if (!wav_.Open(cfg_.wav_path, cfg_.channels, kRate)) {
        Say(2, "cannot open " + cfg_.wav_path + ": the take has no sound");
        port.Disarm();
        return false;
    }
    book_.Begin(cfg_.fps);
    pcm_.clear();
    scratch_.assign(static_cast<size_t>(kMixerFrame) * cfg_.channels, 0.0f);
    frames_ = 0;
    last_row_frames_ = 0;
    preroll_ = static_cast<uint32_t>(thrown);
    late_dropped_ = 0;
    steps_ = 0;
    timeouts_ = 0;
    timeouts_in_a_row_ = 0;
    wait_sum_ms_ = 0;
    wait_max_ms_ = 0;
    dead_ = false;
    wav_failed_logged_ = false;
    step_ = StepRow{};
    armed_ = true;
    return true;
}

// The tickets the book wants for the frames so far, as many as the mixer may have out at once allows (what is out: handed and not yet on the
// timeline, and the late frames still to come and be thrown away). Returns how many went.
uint64_t Session::HandSome() {
    const uint64_t cap = std::max<uint64_t>(port_->Cap(), 1);
    const uint64_t busy = book_.Outstanding() + book_.Skipping();
    if (busy >= cap) return 0;
    const uint64_t give = std::min(book_.Wanted(frames_), cap - busy);
    if (give > 0) {
        port_->Hand(give);
        book_.Released(give);
    }
    return give;
}

// The queue holds `queued` frames: the late ones of an earlier step are pulled and thrown away, this take's go onto the timeline.
void Session::PullQueued(uint64_t queued) {
    const Book::Pop p = book_.Consume(queued);
    for (uint64_t i = 0; i < p.discard + p.use; ++i) {
        const bool used = i >= p.discard;
        const bool pulled = !dead_ && port_->Pull(scratch_.data(), cfg_.channels);
        if (!pulled) {
            if (!dead_) {
                dead_ = true;
                Say(2, "the mixer's output could not be read (the audio device is gone?): the rest of the take's WAV is silence");
            }
            std::fill(scratch_.begin(), scratch_.end(), 0.0f);
        }
        if (used) {
            pcm_.insert(pcm_.end(), scratch_.begin(), scratch_.end());
            ++step_.used;
        } else {
            ++late_dropped_;
            ++step_.discarded;
        }
    }
}

// The port is asked from the game thread at every step and inside the wait, so a close that comes while a step waits for a mixer that is being given back
// ends the wait at the next look (5 ms at the most), not at the step's timeout (2 s, three steps in a row).
bool Session::NoteClosed() {
    if (dead_) return true;
    if (!port_ || !port_->Closed()) return false;
    dead_ = true;
    Say(1, "the game is closing: the rest of the take's WAV is silence");
    return true;
}

void Session::StepBegin(uint64_t video_frames) {
    if (!armed_) return;
    frames_ = std::max(frames_, video_frames);
    step_ = StepRow{};
    if (NoteClosed()) return;
    step_.released += HandSome();
}

void Session::StepEnd() {
    if (!armed_) return;
    const auto t0 = Clock::now();
    bool timed_out = false;
    NoteClosed();
    if (!dead_) {
        const auto deadline = t0 + std::chrono::milliseconds(std::max(cfg_.timeout_ms, 1));
        for (;;) {
            if (NoteClosed()) break;
            const uint64_t queued = port_->Queued();
            const uint64_t before = step_.used + step_.discarded;
            if (queued > 0) PullQueued(queued);
            if (dead_) break;
            const uint64_t more = HandSome();  // the rest of the video frame's mixer frames, when the mixer's room was smaller than they
            step_.released += more;
            if (book_.Outstanding() == 0 && book_.Wanted(frames_) == 0) break;
            const auto now = Clock::now();
            if (now >= deadline) {
                timed_out = true;
                break;
            }
            if (more == 0 && step_.used + step_.discarded == before) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                port_->Wait(static_cast<uint32_t>(std::clamp<long long>(left, 1, kWaitSliceMs)));
            }
        }
        if (timed_out) {
            const uint64_t silent = book_.GiveUp();  // what is outstanding goes onto the timeline as silence; as many frames that come later are thrown away
            step_.silent += silent;
            pcm_.insert(pcm_.end(), static_cast<size_t>(silent) * kMixerFrame * cfg_.channels, 0.0f);
            ++timeouts_;
            ++timeouts_in_a_row_;
            char line[256];
            std::snprintf(line, sizeof line, "video frame %llu: the mixer gave no frame in %d ms; silence in its place (%u step(s) in a row)",
                          static_cast<unsigned long long>(frames_ > 0 ? frames_ - 1 : 0), cfg_.timeout_ms, timeouts_in_a_row_);
            Say(1, line);
            if (timeouts_in_a_row_ >= kTimeoutsToDead && !dead_) {
                dead_ = true;
                std::snprintf(line, sizeof line, "%u steps in a row without the mixer: the rest of the take's WAV is silence", timeouts_in_a_row_);
                Say(2, line);
            }
        } else if (!dead_) {
            timeouts_in_a_row_ = 0;
        }
    }
    const double wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    wait_sum_ms_ += wait_ms;
    wait_max_ms_ = std::max(wait_max_ms_, wait_ms);
    ++steps_;

    // the file: exactly what the video frames so far own
    const uint64_t shortfall = book_.Shortfall(frames_);
    if (shortfall > 0) {
        book_.Pad(shortfall);
        pcm_.insert(pcm_.end(), static_cast<size_t>(shortfall) * cfg_.channels, 0.0f);
    }
    const uint64_t w = std::min<uint64_t>(book_.Writable(frames_), pcm_.size() / cfg_.channels);
    const size_t floats = static_cast<size_t>(w) * cfg_.channels;
    float peak = 0.0f;
    for (size_t i = 0; i < floats; ++i) peak = std::max(peak, std::fabs(pcm_[i]));
    if (w > 0) {
        if (!wav_.Append(pcm_.data(), w) && !wav_failed_logged_) {
            wav_failed_logged_ = true;
            Say(2, "writing " + cfg_.wav_path + " failed (disk full, or the WAV is at its size limit): the rest of the take's sound is lost");
        }
        pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<std::ptrdiff_t>(floats));
    }
    book_.Wrote(w);
    if (frames_ > last_row_frames_) {  // one row for each video frame (a gate that saw no new frame has nothing to say)
        step_.frame = frames_ - 1;
        step_.samples = w;
        step_.cum = book_.Written();
        step_.expected = book_.Target(frames_);
        step_.drift = static_cast<int64_t>(step_.cum) - static_cast<int64_t>(step_.expected);
        step_.residual = book_.Available();
        step_.wait_ms = wait_ms;
        step_.peak = peak;
        last_row_frames_ = frames_;
        if (cfg_.row) cfg_.row(step_);
    }
}

Summary Session::Finish(uint64_t video_frames) {
    Summary s;
    if (!armed_) return s;
    if (video_frames > frames_) {  // frames the gate did not cover
        StepBegin(video_frames);
        StepEnd();
    }
    s.ran = true;
    s.wav = cfg_.wav_path;
    s.channels = cfg_.channels;
    s.fps = cfg_.fps;
    s.video_frames = video_frames;
    s.samples = book_.Written();
    s.expected = SamplesThrough(video_frames, cfg_.fps);
    s.mixer_frames = book_.TotalReleased();
    s.timeouts = timeouts_;
    s.silent_frames = book_.Silent();
    s.late_dropped = late_dropped_;
    s.padded_samples = book_.Padded();
    s.preroll_frames = preroll_;
    s.wait_ms_mean = steps_ ? wait_sum_ms_ / static_cast<double>(steps_) : 0.0;
    s.wait_ms_max = wait_max_ms_;
    s.dead = dead_;
    s.wav_failed = wav_.Failed();
    wav_.Close();
    s.wav_failed = s.wav_failed || wav_.Failed();
    s.ok = s.timeouts == 0 && s.samples == s.expected && !s.wav_failed && s.padded_samples == 0 && !s.dead;
    armed_ = false;
    port_->Disarm();
    return s;
}

bool Session::CloseWavForExit(uint64_t* sample_frames) {
    if (!wav_.IsOpen()) return false;
    wav_.Close();
    if (sample_frames) *sample_frames = wav_.Frames();
    return true;
}

}  // namespace mc::cinematic::capture_audio
