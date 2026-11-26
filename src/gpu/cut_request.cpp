// Camera cuts the game asked for, and the motion blur on a cut: see cut_request.h.

#include "cut_request.h"
#include "cascade_schedule.h"
#include "settings/immortal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mcla::gpu::cut {

namespace {

double Dot3(const double* a, const double* b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The unit vector of a vector, false when it has no direction.
bool Unit3(double* v) {
  const double l = std::sqrt(Dot3(v, v));
  if (!(l > 1e-6) || !std::isfinite(l)) {
    return false;
  }
  v[0] /= l;
  v[1] /= l;
  v[2] /= l;
  return true;
}

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  const uint8_t* p = bank + reg * 16 + comp * 4;
  const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

}  // namespace

bool SameCamera(const CameraPose& a, const CameraPose& b) {
  double d[3] = {a.position[0] - b.position[0], a.position[1] - b.position[1], a.position[2] - b.position[2]};
  if (!(std::sqrt(Dot3(d, d)) < kSamePosition)) {
    return false;
  }
  return Dot3(a.forward, b.forward) >= kSameCosine && Dot3(a.up, b.up) >= kSameCosine;
}

uint64_t Requests::Submit(uint32_t game_frame, const CameraPose& camera) {
  cascade_schedule::camera_cut.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mutex_);
  constexpr size_t kMaxPending = 16;
  if (pending_.size() >= kMaxPending) {  // nothing is taking them: drop the oldest, it is long over
    pending_.erase(pending_.begin());
    ++counts_.expired;
  }
  Entry e;
  e.serial = next_serial_++;
  e.game_frame = game_frame;
  e.camera = camera;
  pending_.push_back(e);
  ++counts_.submitted;
  pending_count_.store(uint32_t(pending_.size()), std::memory_order_relaxed);
  return e.serial;
}

Requests::Taken Requests::Take(const CameraPose& now) {
  Taken out;
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_.empty()) {
    return out;
  }
  for (Entry& e : pending_) {
    ++e.age;
  }
  // The oldest request this camera is the frame of.
  size_t hit = pending_.size();
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (SameCamera(pending_[i].camera, now)) {
      hit = i;
      break;
    }
  }
  if (hit < pending_.size()) {
    const Entry e = pending_[hit];
    out.hit = true;
    out.serial = e.serial;
    out.game_frame = e.game_frame;
    out.age = e.age;
    out.skipped = uint32_t(hit);
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(hit) + 1);
    ++counts_.matched;
    counts_.skipped += hit;
  }
  // What nothing has matched for too long.
  for (size_t i = 0; i < pending_.size();) {
    if (pending_[i].age > kGiveUpAfter) {
      if (out.expired == 0) {
        out.expired_serial = pending_[i].serial;
        out.expired_game_frame = pending_[i].game_frame;
      }
      ++out.expired;
      ++counts_.expired;
      pending_.erase(pending_.begin() + std::ptrdiff_t(i));
    } else {
      ++i;
    }
  }
  pending_count_.store(uint32_t(pending_.size()), std::memory_order_relaxed);
  return out;
}

Requests::Counts Requests::counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}

Requests& Global() {
  static Requests& requests = larecomp::Leak<Requests>();
  return requests;
}

bool CameraFromGuestBank(const uint8_t* ps_bank, CameraPose* out) {
  double up[3];
  double back[3];
  double pos[3];
  for (uint32_t c = 0; c < 3; ++c) {
    up[c] = double(BankFloat(ps_bank, 13, c));
    back[c] = double(BankFloat(ps_bank, 14, c));
    pos[c] = double(BankFloat(ps_bank, 15, c));
    if (!std::isfinite(up[c]) || !std::isfinite(back[c]) || !std::isfinite(pos[c])) {
      return false;
    }
  }
  if (!Unit3(up) || !Unit3(back)) {
    return false;
  }
  CameraPose p;
  for (int c = 0; c < 3; ++c) {
    p.position[c] = pos[c];
    p.forward[c] = -back[c];
    p.up[c] = up[c];
  }
  *out = p;
  return true;
}

bool NeutralizeMotionBlur(float* host_bank) {
  const float width = host_bank[62 * 4 + 0];
  const float height = host_bank[62 * 4 + 1];
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    return false;
  }
  double m[4][4];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      m[r][c] = double(host_bank[(8 + r) * 4 + c]);
      if (!std::isfinite(m[r][c])) {
        return false;
      }
    }
  }
  const double hw = double(width) * 0.5;
  const double hh = double(height) * 0.5;
  // View-projection times the pixel matrix [hw 0 0 0; 0 -hh 0 0; 0 0 1 0; hw hh 0 1], row vectors.
  float out[4][4];
  for (int r = 0; r < 4; ++r) {
    out[r][0] = float(m[r][0] * hw + m[r][3] * hw);
    out[r][1] = float(-m[r][1] * hh + m[r][3] * hh);
    out[r][2] = float(m[r][2]);
    out[r][3] = float(m[r][3]);
  }
  for (uint32_t k = 0; k < kBlurSlots; ++k) {
    for (uint32_t r = 0; r < 4; ++r) {
      std::memcpy(host_bank + (kBlurFirstRegister + 4 * k + r) * 4, out[r], sizeof(out[r]));
    }
  }
  return true;
}

}  // namespace mcla::gpu::cut
