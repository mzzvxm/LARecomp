// Check of a take's sound, the pure half (capture_audio_core.h): the samples a number of video frames owns, the mixer frames that cover
// them, the books of what was released, what arrived, what was given up on and what was written, and the WAV file. A program of its
// own: it needs no game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_capture_audio_test      (CMakeLists.txt)
//   <build dir>/larecomp_capture_audio_test
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 -D_CRT_SECURE_NO_WARNINGS src/mc_engine/cinematic/capture_audio_test.cpp src/mc_engine/cinematic/capture_audio_core.cpp -o capture_audio_test.exe
//
// The part that matters is TestTake: a take of video frames is replayed against a mixer that answers a credit with a frame, the way the
// glue runs it (release what the book wants, in chunks of what the semaphore can hold, consume what arrived, write what the target
// allows), at rates from 10 to 1000 fps for minutes of video, and after every frame the file holds exactly the video's samples.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "capture_audio_core.h"

using namespace mc::cinematic::capture_audio;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckU(uint64_t got, uint64_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    }
}

void CheckNum(double got, double want, const std::string& what, double tol) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

// ---------------------------------------------------------------------------------------------------------------- the driver's install

// The off path. The capture driver is put in front of the game's audio output only when a take that records sound starts: a game with no
// take, and a take with the sound off, leave the output exactly as it is (PlanDriver says kNone, so Begin installs and arms nothing).
void TestInstallPlan() {
    Check(PlanDriver(false, false) == DriverPlan::kNone, "no sound asked, no driver: nothing to do");
    Check(PlanDriver(false, true) == DriverPlan::kNone, "no sound asked, a driver from an earlier take: still nothing is touched");
    Check(PlanDriver(true, false) == DriverPlan::kInstallThenArm, "sound asked, no driver: install it, then arm it");
    Check(PlanDriver(true, true) == DriverPlan::kArm, "sound asked, driver there: arm it, install nothing");

    // A session in the order the game runs it, with Begin's rule applied to a model of the audio output: `wrapped` is whether the capture
    // driver is in the path, `installs` how many times it was put there.
    struct Output {
        bool wrapped = false;
        int installs = 0;
        int armed = 0;
        // capture_audio.cpp Begin's switch on the plan
        bool Begin(bool records_sound) {
            switch (PlanDriver(records_sound, wrapped)) {
                case DriverPlan::kNone:
                    return false;
                case DriverPlan::kInstallThenArm:
                    wrapped = true;
                    ++installs;
                    [[fallthrough]];
                case DriverPlan::kArm:
                    ++armed;
                    return true;
            }
            return false;
        }
    };
    Output o;
    // (a game that runs no take never calls Begin, so it stays here)
    Check(!o.wrapped && o.installs == 0, "at the start of the game the output is the real one, with no wrapper");
    Check(!o.Begin(false) && !o.wrapped && o.installs == 0 && o.armed == 0, "a take with the sound off leaves the output alone");
    Check(!o.Begin(false) && !o.wrapped, "and so does the next one");
    Check(o.Begin(true) && o.wrapped && o.installs == 1 && o.armed == 1, "the first take with sound puts the driver in front of the output and arms it");
    Check(o.Begin(true) && o.installs == 1 && o.armed == 2, "the second one arms the same driver (no second install)");
    Check(!o.Begin(false) && o.wrapped && o.installs == 1 && o.armed == 2, "a take with the sound off after that arms nothing and installs nothing");
}

// ---------------------------------------------------------------------------------------------------------------- the timeline

void TestSamples() {
    CheckU(SamplesThrough(0, 60.0), 0, "no frames own no samples");
    CheckU(SamplesThrough(1, 60.0), 800, "a frame at 60 fps is 800 samples");
    CheckU(SamplesThrough(3600, 60.0), 2880000, "60 s at 60 fps is 2,880,000 samples");
    CheckU(SamplesThrough(1800, 60.0), 1440000, "30 s at 60 fps");
    CheckU(SamplesThrough(1, 24.0), 2000, "24 fps: 2000");
    CheckU(SamplesThrough(1, 30.0), 1600, "30 fps: 1600");
    CheckU(SamplesThrough(1, 120.0), 400, "120 fps: 400");
    CheckU(SamplesThrough(1, 240.0), 200, "240 fps: 200");
    CheckU(SamplesThrough(1, 10.0), 4800, "10 fps: 4800");
    CheckU(SamplesThrough(1, 1000.0), 48, "1000 fps: 48");
    CheckU(SamplesThrough(1, 59.94), 801, "59.94 fps: 800.8 rounds to 801");
    CheckU(SamplesThrough(5, 59.94), 4004, "59.94 fps: five frames are 4004 exactly");
    CheckU(SamplesThrough(10, 0.0), 0, "a rate of 0 is not a rate");
    CheckU(SamplesThrough(10, -3.0), 0, "a negative rate");
    CheckU(SamplesThrough(10, std::nan("")), 0, "NaN");
    CheckU(MixerFramesCovering(0), 0, "no samples need no frame");
    CheckU(MixerFramesCovering(1), 1, "one sample needs a frame");
    CheckU(MixerFramesCovering(256), 1, "256 samples are one frame");
    CheckU(MixerFramesCovering(257), 2, "257 need two");
    CheckU(MixerFramesCovering(800), 4, "800 samples (a frame at 60 fps) need four mixer frames (3.125)");
    CheckU(MixerFramesCovering(2880000), 11250, "60 s at 60 fps is 11250 mixer frames exactly");

    // no drift: the count is the exact ratio rounded, at every frame, for any length and any rate
    for (double fps : {10.0, 12.0, 23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 59.94, 60.0, 72.0, 90.0, 100.0, 120.0, 143.99, 144.0, 239.76, 240.0, 1000.0}) {
        const uint64_t spf_lo = static_cast<uint64_t>(std::floor(48000.0 / fps));
        const uint64_t spf_hi = static_cast<uint64_t>(std::ceil(48000.0 / fps));
        uint64_t prev = 0;
        bool steps_ok = true;
        bool near_ok = true;
        const uint64_t frames = static_cast<uint64_t>(fps * 600.0);  // ten minutes of video
        for (uint64_t n = 1; n <= frames; ++n) {
            const uint64_t s = SamplesThrough(n, fps);
            const uint64_t step = s - prev;
            if (step < spf_lo || step > spf_hi) steps_ok = false;
            const long double exact = static_cast<long double>(n) * 48000.0L / static_cast<long double>(fps);
            if (std::fabs(static_cast<double>(static_cast<long double>(s) - exact)) > 0.5 + 1e-6) near_ok = false;
            prev = s;
        }
        char what[96];
        std::snprintf(what, sizeof what, "%g fps: every frame owns floor or ceil of 48000/fps samples", fps);
        Check(steps_ok, what);
        std::snprintf(what, sizeof what, "%g fps: ten minutes, always within half a sample of the exact length", fps);
        Check(near_ok, what);
    }
    // a day of 60 fps video: still exact (the product is held in a long double)
    CheckU(SamplesThrough(60ull * 3600 * 24 * 60, 60.0), 48000ull * 3600 * 24 * 60, "a day of video at 60 fps");
}

// ---------------------------------------------------------------------------------------------------------------- the take

// What the glue does every video frame, with a mixer that answers a credit by a frame. `chunk` is what the semaphore can hold (the
// SDK's queue length): credits are released in pieces of at most that. `late(k)`: the number of the step's frames that do not arrive
// in this step (they arrive in the next one, after the step gave up on them).
struct Replay {
    Book book;
    std::deque<int> queue;      // frames the mixer made, waiting
    std::vector<int> arriving;  // frames that will be in the queue when the next step starts (late ones)
    uint64_t chunk = 8;
    uint64_t held = 8;          // credits we hold
    uint64_t written = 0;
    uint64_t max_residual = 0;
    uint64_t discarded = 0;
    uint64_t silent_frames = 0;
    bool ok = true;

    void Begin(double fps, uint64_t credits) {
        book.Begin(fps);
        held = chunk = credits;
    }
    // One step: `n` frames of video exist now. `late`: how many of the frames released in this step do not come back in time.
    void Step(uint64_t n, uint64_t late) {
        // frames that were late in the step before are in the queue now
        for (int f : arriving) queue.push_back(f);
        arriving.clear();
        {  // the glue throws the late frames away and takes the credits back before it releases anything
            const Book::Pop p = book.Consume(queue.size());
            for (uint64_t i = 0; i < p.discard + p.use; ++i) queue.pop_front();
            held += p.discard + p.use;
            discarded += p.discard;
        }
        uint64_t late_left = late;
        for (int guard = 0; guard < 100000; ++guard) {
            const uint64_t want = book.Wanted(n);
            const uint64_t give = std::min(want, held);
            if (give == 0) break;
            book.Released(give);
            held -= give;
            // the mixer answers each credit with a frame: the late ones come in the next step
            for (uint64_t i = 0; i < give; ++i) {
                if (late_left > 0) {
                    --late_left;
                    arriving.push_back(1);
                } else {
                    queue.push_back(1);
                }
            }
            const Book::Pop p = book.Consume(queue.size());
            for (uint64_t i = 0; i < p.discard + p.use; ++i) queue.pop_front();
            held += p.discard + p.use;
            discarded += p.discard;
            if (book.Outstanding() > 0) {
                // some did not come: this step gives up on them
                silent_frames += book.GiveUp();
            }
        }
        // anything the loop left outstanding (late frames of the last chunk) is given up on
        if (book.Outstanding() > 0) silent_frames += book.GiveUp();
        const uint64_t short_by = book.Shortfall(n);
        if (short_by > 0) book.Pad(short_by);
        const uint64_t w = book.Writable(n);
        book.Wrote(w);
        written += w;
        max_residual = std::max(max_residual, book.Available());
        if (written != book.Target(n)) ok = false;
    }
};

void TestTake() {
    // 60 fps, one minute, an SDK queue of 8: every frame takes 3 or 4 mixer frames, the file is exact after every frame
    {
        Replay r;
        r.Begin(60.0, 8);
        for (uint64_t n = 1; n <= 3600; ++n) {
            r.Step(n, 0);
            if (!r.ok) {
                Check(false, "60 fps: the file holds the video's samples after every frame (broke at frame " + std::to_string(n) + ")");
                break;
            }
        }
        CheckU(r.written, 2880000, "60 fps, 60 s: the file holds 2,880,000 samples");
        CheckU(r.book.TotalReleased(), 11250, "60 fps, 60 s: 11250 mixer frames were released, no more");
        CheckU(r.book.Counted(), 11250, "60 fps, 60 s: all of them are on the timeline");
        Check(r.max_residual > 0 && r.max_residual < kMixerFrame, "60 fps: 800 = 3.125 mixer frames, so the mixer is ahead by a tail of under 256 samples");
        CheckU(r.discarded, 0, "nothing thrown away");
        CheckU(r.silent_frames, 0, "no silence");
        CheckU(r.book.Padded(), 0, "no padding");
    }
    // the rates, with a queue of 4 (the smallest the SDK makes) and of 8 and 64
    for (double fps : {10.0, 12.0, 23.976, 24.0, 29.97, 30.0, 59.94, 60.0, 120.0, 144.0, 240.0, 1000.0}) {
        for (uint64_t credits : {4ull, 8ull, 64ull}) {
            Replay r;
            r.Begin(fps, credits);
            const uint64_t frames = static_cast<uint64_t>(fps * 90.0);
            uint64_t bad_at = 0;
            for (uint64_t n = 1; n <= frames; ++n) {
                r.Step(n, 0);
                if (!r.ok) {
                    bad_at = n;
                    break;
                }
                if (r.book.Available() >= kMixerFrame) {
                    bad_at = n;
                    r.ok = false;
                    break;
                }
            }
            char what[160];
            std::snprintf(what, sizeof what, "%g fps, queue %llu: the file is exact after every frame and the tail stays under a mixer frame (bad at %llu)", fps,
                          static_cast<unsigned long long>(credits), static_cast<unsigned long long>(bad_at));
            Check(r.ok, what);
            const uint64_t end = SamplesThrough(frames, fps);
            std::snprintf(what, sizeof what, "%g fps, queue %llu: 90 s ends at the exact sample", fps, static_cast<unsigned long long>(credits));
            CheckU(r.written, end, what);
            std::snprintf(what, sizeof what, "%g fps, queue %llu: only the frames needed were released", fps, static_cast<unsigned long long>(credits));
            CheckU(r.book.TotalReleased(), MixerFramesCovering(end), what);
            Check(r.book.Padded() == 0 && r.silent_frames == 0 && r.discarded == 0, "an ideal mixer: no padding, no silence, nothing thrown away");
        }
    }
    // the first frames: the first video frame needs 4 mixer frames at 60 fps, its tail (32 samples) opens the second
    {
        Book b;
        b.Begin(60.0);
        CheckU(b.Wanted(1), 4, "frame 1 at 60 fps wants 4 mixer frames");
        b.Released(4);
        CheckU(b.Wanted(1), 0, "and not again");
        CheckU(b.QueueNeeded(), 4, "the queue has to hold those 4");
        const Book::Pop p = b.Consume(4);
        CheckU(p.use, 4, "four are this take's");
        CheckU(p.discard, 0, "none thrown away");
        CheckU(b.Available(), 1024, "1024 samples on the timeline");
        CheckU(b.Writable(1), 800, "800 are frame 1's");
        b.Wrote(800);
        CheckU(b.Available(), 224, "224 wait for frame 2");
        CheckU(b.Wanted(2), 3, "frame 2 (1600 samples = 6.25 mixer frames: 7) wants 3 more");
        b.Released(3);
        CheckU(b.Consume(3).use, 3, "they come");
        CheckU(b.Writable(2), 800, "800 more samples are due");
        b.Wrote(800);
        CheckU(b.Available(), 192, "7 frames = 1792 samples, 1600 written: 192 wait for frame 3");
    }
}

// A frame that never came: the timeline gets silence for it, the next frame that comes is thrown away, the length stays exact, and the
// sound after the stall is where it was without it.
void TestLate() {
    Replay r;
    r.Begin(60.0, 8);
    uint64_t bad = 0;
    for (uint64_t n = 1; n <= 600; ++n) {
        // frames 100, 101 and 300 each lose two mixer frames to a stall
        const uint64_t late = (n == 100 || n == 101 || n == 300) ? 2 : 0;
        r.Step(n, late);
        if (!r.ok && !bad) bad = n;
    }
    CheckU(bad, 0, "late frames: the file holds the video's samples after every frame");
    CheckU(r.written, 480000, "10 s at 60 fps with three stalls: still 480,000 samples");
    CheckU(r.silent_frames, 6, "three stalls of two mixer frames: six frames of silence");
    CheckU(r.discarded, 6, "and the six that came late were thrown away");
    CheckU(r.book.Skipping(), 0, "nothing left to throw away");
    CheckU(r.book.TotalReleased(), MixerFramesCovering(480000), "the number released is the number covering the video, silent ones included");
    CheckU(r.book.Padded(), 0, "no padding was needed");

    // a mixer that has stopped for good (no frame ever comes and no credit comes back): the file is padded with silence to its length
    Book b;
    b.Begin(60.0);
    uint64_t written = 0;
    uint64_t held = 8;
    for (uint64_t n = 1; n <= 60; ++n) {
        const uint64_t give = std::min<uint64_t>(b.Wanted(n), held);
        b.Released(give);
        held -= give;
        if (b.Outstanding() > 0) b.GiveUp();
        const uint64_t shortfall = b.Shortfall(n);
        if (shortfall) b.Pad(shortfall);
        const uint64_t w = b.Writable(n);
        b.Wrote(w);
        written += w;
        if (written != b.Target(n)) {
            Check(false, "a dead mixer: the file is exact after every frame (frame " + std::to_string(n) + ")");
            break;
        }
    }
    CheckU(written, 48000, "a dead mixer: one second of video, one second of samples");
    Check(b.Padded() > 0, "a dead mixer: the rest is padding");
}

// ---------------------------------------------------------------------------------------------------------------- the WAV

uint32_t Get32(const std::vector<uint8_t>& v, size_t at) {
    return static_cast<uint32_t>(v[at]) | (static_cast<uint32_t>(v[at + 1]) << 8) | (static_cast<uint32_t>(v[at + 2]) << 16) |
           (static_cast<uint32_t>(v[at + 3]) << 24);
}
uint32_t Get16(const std::vector<uint8_t>& v, size_t at) {
    return static_cast<uint32_t>(v[at]) | (static_cast<uint32_t>(v[at + 1]) << 8);
}
bool Tag(const std::vector<uint8_t>& v, size_t at, const char* t) {
    return std::memcmp(v.data() + at, t, 4) == 0;
}

void TestHeader() {
    const std::vector<uint8_t> h = WavHeader(2, 48000, 1440000);
    CheckU(h.size(), 56, "a stereo header is 56 bytes");
    CheckU(h.size(), WavHeaderSize(2), "WavHeaderSize says so");
    Check(Tag(h, 0, "RIFF") && Tag(h, 8, "WAVE") && Tag(h, 12, "fmt ") && Tag(h, 36, "fact") && Tag(h, 48, "data"), "the chunks in order: RIFF WAVE fmt fact data");
    CheckU(Get32(h, 4), 56 - 8 + 1440000u * 8, "RIFF size = the file's size minus 8");
    CheckU(Get32(h, 16), 16, "a plain fmt chunk");
    CheckU(Get16(h, 20), 3, "format 3: IEEE float");
    CheckU(Get16(h, 22), 2, "two channels");
    CheckU(Get32(h, 24), 48000, "48 kHz");
    CheckU(Get32(h, 28), 48000 * 8, "bytes a second");
    CheckU(Get16(h, 32), 8, "block align");
    CheckU(Get16(h, 34), 32, "32 bits");
    CheckU(Get32(h, 40), 4, "fact: 4 bytes");
    CheckU(Get32(h, 44), 1440000, "fact: sample frames");
    CheckU(Get32(h, 52), 1440000u * 8, "data size");

    const std::vector<uint8_t> x = WavHeader(6, 48000, 100);
    CheckU(x.size(), 80, "a 6 channel header is 80 bytes");
    CheckU(x.size(), WavHeaderSize(6), "WavHeaderSize says so");
    Check(Tag(x, 0, "RIFF") && Tag(x, 12, "fmt ") && Tag(x, 60, "fact") && Tag(x, 72, "data"), "the chunks of the extensible header");
    CheckU(Get32(x, 16), 40, "extensible fmt chunk");
    CheckU(Get16(x, 20), 0xFFFE, "WAVE_FORMAT_EXTENSIBLE");
    CheckU(Get16(x, 22), 6, "six channels");
    CheckU(Get32(x, 28), 48000 * 24, "bytes a second");
    CheckU(Get16(x, 32), 24, "block align");
    CheckU(Get16(x, 36), 22, "cbSize");
    CheckU(Get32(x, 40), 0x3F, "5.1 speaker mask: fl fr fc lfe bl br");
    CheckU(Get32(x, 44), 3, "the subformat is IEEE float");
    CheckU(Get32(x, 76), 100u * 24, "data size");
    CheckU(Get32(x, 4), 80 - 8 + 100u * 24, "RIFF size");

    const std::vector<uint8_t> big = WavHeader(2, 48000, 1ull << 40);
    CheckU(Get32(big, 52), 0xFFFFFFFFu, "a size beyond 4 GiB saturates (no RF64)");
}

void TestFile() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "larecomp_capture_audio_test.wav";
    {
        WavFile w;
        Check(w.Open(path.string(), 2, 48000), "the file opens");
        Check(w.IsOpen(), "it is open");
        std::vector<float> a(800 * 2), b(801 * 2);
        for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<float>(i) * 0.001f - 0.5f;
        for (size_t i = 0; i < b.size(); ++i) b[i] = 1.5f - static_cast<float>(i) * 0.002f;  // beyond 1.0 is kept (float WAV)
        Check(w.Append(a.data(), 800), "an append");
        Check(w.Append(nullptr, 0), "an empty append is fine");
        Check(w.Append(b.data(), 801), "another");
        CheckU(w.Frames(), 1601, "1601 sample frames");
        Check(w.Close(), "it closes");
        Check(!w.IsOpen(), "closed");
        CheckU(w.Frames(), 1601, "the count stays");
    }
    // read it back
    std::vector<uint8_t> bytes;
    if (std::FILE* f = std::fopen(path.string().c_str(), "rb")) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    CheckU(bytes.size(), 56 + 1601 * 8, "the file's size is the header and the samples");
    if (bytes.size() >= 56) {
        CheckU(Get32(bytes, 4), bytes.size() - 8, "RIFF size is the file's size minus 8 after the close");
        CheckU(Get32(bytes, 44), 1601, "fact holds the count");
        CheckU(Get32(bytes, 52), 1601 * 8, "the data size is fixed up");
        float first = 0, last = 0, mid = 0;
        std::memcpy(&first, bytes.data() + 56, 4);
        std::memcpy(&mid, bytes.data() + 56 + 800 * 8, 4);
        std::memcpy(&last, bytes.data() + bytes.size() - 4, 4);
        CheckNum(first, -0.5, "the first sample", 1e-6);
        CheckNum(mid, 1.5, "the first sample of the second append (a value above 1.0 is not clipped)", 1e-6);
        CheckNum(last, 1.5 - 1601.0 * 0.002, "the last sample", 1e-3);
    }
    // an unopenable path
    {
        WavFile w;
        const std::filesystem::path bad = std::filesystem::temp_directory_path() / "larecomp_no_such_folder_m2c" / "x.wav";
        Check(!w.Open(bad.string(), 2, 48000), "a path in a folder that does not exist does not open");
        Check(w.Failed(), "and says so");
        Check(!w.Append(nullptr, 1), "no append to nothing");
    }
    // six channels
    {
        WavFile w;
        Check(w.Open(path.string(), 6, 48000), "a 6 channel file opens");
        std::vector<float> a(256 * 6, 0.25f);
        Check(w.Append(a.data(), 256), "one mixer frame");
        Check(w.Close(), "closes");
        std::vector<uint8_t> six;
        if (std::FILE* f = std::fopen(path.string().c_str(), "rb")) {
            uint8_t buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) six.insert(six.end(), buf, buf + n);
            std::fclose(f);
        }
        CheckU(six.size(), 80 + 256 * 24, "size of the 6 channel file");
        if (six.size() >= 80) CheckU(Get32(six, 76), 256 * 24, "its data size");
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    TestInstallPlan();
    TestSamples();
    TestTake();
    TestLate();
    TestHeader();
    TestFile();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
