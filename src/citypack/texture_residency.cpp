#include "citypack/texture_residency.h"

#include <algorithm>
#include <limits>

namespace larecomp::citypack {

uint32_t FirstResidentMip(const HitTexture& texture, uint32_t cap_px) {
  if (cap_px == 0 || texture.mip_count <= 1) return 0;
  for (uint32_t m = 0; m < texture.mip_count; ++m) {
    const uint32_t side = std::max(std::max(uint32_t(texture.width) >> m, 1u), std::max(uint32_t(texture.height) >> m, 1u));
    if (side <= cap_px) return m;
  }
  return texture.mip_count - 1;
}

uint64_t ResidentBytes(const HitTexture& texture, uint32_t first_mip) { return texture.ChainBytes(first_mip); }

uint64_t AccountedBytes(const HitTexture& texture, uint32_t first_mip, uint64_t alignment) {
  const uint64_t bytes = texture.ChainBytes(first_mip);
  if (alignment <= 1) return bytes;
  return (bytes + alignment - 1) / alignment * alignment;
}

std::vector<UploadLevel> PlanUploadLevels(const HitTexture& texture, uint32_t first_mip, uint32_t pitch_alignment) {
  std::vector<UploadLevel> levels;
  if (first_mip >= texture.mip_count) return levels;
  const bool raw = texture.format == HitTextureFormat::kRgba8;
  const uint32_t side = raw ? 1u : 4u;
  const uint32_t block_bytes = raw ? 4u : (texture.format == HitTextureFormat::kBc1 ? 8u : 16u);
  const uint32_t alignment = std::max(pitch_alignment, 1u);
  uint64_t source = 0;
  for (uint32_t m = first_mip; m < texture.mip_count; ++m) {
    UploadLevel l;
    l.width = std::max(uint32_t(texture.width) >> m, 1u);
    l.height = std::max(uint32_t(texture.height) >> m, 1u);
    l.rows = (l.height + side - 1) / side;
    l.row_bytes = ((l.width + side - 1) / side) * block_bytes;
    l.pitch = (l.row_bytes + alignment - 1) / alignment * alignment;
    l.source_offset = source;
    source += uint64_t(l.row_bytes) * l.rows;
    levels.push_back(l);
  }
  return levels;
}

ResidentTotals TotalResidentBytes(const std::vector<HitTexture>& table, uint32_t cap_px) {
  ResidentTotals totals;
  for (const HitTexture& t : table) {
    const uint32_t first = FirstResidentMip(t, cap_px);
    totals.bytes += ResidentBytes(t, first);
    ++totals.textures;
    if (first != 0) ++totals.capped;
  }
  return totals;
}

std::vector<uint16_t> CollectTextures(const std::vector<HitRun>& runs, const std::vector<HitMaterial>& materials) {
  std::vector<uint16_t> out;
  for (const HitRun& run : runs) {
    if (run.material >= materials.size()) continue;
    const HitMaterial& m = materials[run.material];
    if (m.texture != kNoTexture) out.push_back(m.texture);
    if (m.layer != kNoTexture) out.push_back(m.layer);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

TextureResidency::TextureResidency(const std::vector<HitTexture>& table) : table_(table), states_(table.size()) {}

void TextureResidency::Configure(const ResidencyConfig& config) {
  if (config.cap_px != config_.cap_px) config_changed_ = true;
  config_ = config;
}

void TextureResidency::AddSector(uint32_t row, const uint16_t* textures, size_t count, float priority) {
  Sector& s = sectors_[row];
  s.textures.assign(textures, textures + count);
  s.priority = priority;
  dirty_ = true;
}

void TextureResidency::SetSectorPriority(uint32_t row, float priority) {
  const auto it = sectors_.find(row);
  if (it == sectors_.end() || it->second.priority == priority) return;
  it->second.priority = priority;
  dirty_ = true;
}

void TextureResidency::RemoveSector(uint32_t row) {
  if (sectors_.erase(row) != 0) dirty_ = true;
}

void TextureResidency::Recompute() {
  for (Tex& t : states_) {
    t.refs = 0;
    t.priority = std::numeric_limits<float>::max();
  }
  for (const auto& [row, sector] : sectors_) {
    for (uint16_t index : sector.textures) {
      if (index >= states_.size()) continue;
      Tex& t = states_[index];
      ++t.refs;
      t.priority = std::min(t.priority, sector.priority);
    }
  }
  dirty_ = false;
}

uint64_t TextureResidency::PlannedBytes(uint32_t index) const { return AccountedBytes(table_[index], FirstResidentMip(table_[index], config_.cap_px), config_.allocation_alignment); }

void TextureResidency::Drop(uint32_t index, Plan* plan) {
  Tex& t = states_[index];
  switch (t.state) {
    case TextureState::kResident:
      resident_ -= t.bytes;
      --resident_count_;
      break;
    case TextureState::kReady:
      ready_ -= t.bytes;
      break;
    default: return;
  }
  committed_ -= t.bytes;
  t.state = TextureState::kAbsent;
  t.bytes = 0;
  plan->evictions.push_back(index);
  ++evicted_total_;
}

bool TextureResidency::MakeRoom(uint64_t need, float priority, Plan* plan) {
  while (committed_ + need > config_.budget_bytes) {
    // 1. a resident texture no sector references, the one unused for longest
    uint32_t victim = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < states_.size(); ++i) {
      const Tex& t = states_[i];
      if (t.state != TextureState::kResident || t.refs != 0) continue;
      if (victim == 0xFFFFFFFFu || t.last_wanted < states_[victim].last_wanted) victim = i;
    }
    // 2. else a referenced one that is less wanted than the texture that waits (the largest priority number)
    if (victim == 0xFFFFFFFFu) {
      for (uint32_t i = 0; i < states_.size(); ++i) {
        const Tex& t = states_[i];
        if (t.state != TextureState::kResident || t.priority <= priority) continue;
        if (victim == 0xFFFFFFFFu || t.priority > states_[victim].priority) victim = i;
      }
    }
    if (victim == 0xFFFFFFFFu) return false;
    Drop(victim, plan);
  }
  return true;
}

TextureResidency::Plan TextureResidency::Step(uint64_t frame) {
  Plan plan;
  frame_ = frame;
  if (dirty_) Recompute();
  for (Tex& t : states_) {
    if (t.refs != 0) t.last_wanted = frame;
  }

  // A changed cap: what is held at another first level is given back (a read in flight is dropped when it lands).
  if (config_changed_) {
    for (uint32_t i = 0; i < states_.size(); ++i) {
      Tex& t = states_[i];
      if (t.state == TextureState::kAbsent) continue;
      if (t.first_mip == FirstResidentMip(table_[i], config_.cap_px)) continue;
      if (t.state == TextureState::kReading) t.stale = true;
      else Drop(i, &plan);
    }
    config_changed_ = false;
  }

  // Idle: a texture no sector references is kept for a while, then given back whether or not the room is needed.
  for (uint32_t i = 0; i < states_.size(); ++i) {
    const Tex& t = states_[i];
    if (t.state == TextureState::kResident && t.refs == 0 && frame >= t.last_wanted + config_.idle_frames) Drop(i, &plan);
    else if (t.state == TextureState::kReady && t.refs == 0) Drop(i, &plan);  // read for a sector that left
  }

  // The textures that are wanted and absent, the nearest sector first.
  std::vector<uint32_t> wanted;
  for (uint32_t i = 0; i < states_.size(); ++i) {
    const Tex& t = states_[i];
    if (t.refs != 0 && t.state == TextureState::kAbsent && t.retry_at <= frame) wanted.push_back(i);
  }
  std::sort(wanted.begin(), wanted.end(), [&](uint32_t a, uint32_t b) {
    if (states_[a].priority != states_[b].priority) return states_[a].priority < states_[b].priority;
    return a < b;
  });
  for (uint32_t index : wanted) {
    if (reads_in_flight_ >= config_.max_reads_in_flight || reading_ + ready_ >= config_.max_ready_bytes) break;
    Tex& t = states_[index];
    const uint32_t first = FirstResidentMip(table_[index], config_.cap_px);
    const uint64_t need = AccountedBytes(table_[index], first, config_.allocation_alignment);
    if (config_.budget_bytes != 0 && need > config_.budget_bytes) continue;  // can never fit
    if (config_.budget_bytes != 0 && committed_ + need > config_.budget_bytes && !MakeRoom(need, t.priority, &plan)) continue;
    t.state = TextureState::kReading;
    t.first_mip = first;
    t.bytes = need;
    t.stale = false;
    reading_ += need;
    committed_ += need;
    ++reads_in_flight_;
    plan.reads.push_back({index, first, ResidentBytes(table_[index], first)});
  }

  // The uploads of this frame: the textures whose bytes are read, nearest first, at most the frame's bytes (and one at least).
  std::vector<uint32_t> ready;
  for (uint32_t i = 0; i < states_.size(); ++i) {
    if (states_[i].state == TextureState::kReady) ready.push_back(i);
  }
  std::sort(ready.begin(), ready.end(), [&](uint32_t a, uint32_t b) {
    if (states_[a].priority != states_[b].priority) return states_[a].priority < states_[b].priority;
    return a < b;
  });
  uint64_t taken = 0;
  for (uint32_t index : ready) {
    Tex& t = states_[index];
    if (taken != 0 && taken + t.bytes > config_.upload_bytes_per_frame) break;
    taken += t.bytes;
    ready_ -= t.bytes;
    resident_ += t.bytes;
    ++resident_count_;
    t.state = TextureState::kResident;
    plan.uploads.push_back(index);
  }
  return plan;
}

bool TextureResidency::ReadFinished(uint32_t index, bool ok) {
  Tex& t = states_[index];
  if (t.state != TextureState::kReading) return false;
  --reads_in_flight_;
  reading_ -= t.bytes;
  if (!ok || t.stale) {
    committed_ -= t.bytes;
    t.state = TextureState::kAbsent;
    t.bytes = 0;
    t.stale = false;
    if (!ok) {
      ++failed_reads_;
      t.retry_at = frame_ + config_.retry_frames;
    }
    return false;
  }
  t.state = TextureState::kReady;
  ready_ += t.bytes;
  return true;
}

void TextureResidency::CreationFailed(uint32_t index) {
  Tex& t = states_[index];
  if (t.state != TextureState::kResident) return;
  resident_ -= t.bytes;
  --resident_count_;
  committed_ -= t.bytes;
  t.state = TextureState::kAbsent;
  t.bytes = 0;
  t.retry_at = frame_ + config_.retry_frames;
  ++failed_creations_;
}

void TextureResidency::UploadFailed(uint32_t index) {
  Tex& t = states_[index];
  if (t.state != TextureState::kResident) return;
  resident_ -= t.bytes;
  --resident_count_;
  ready_ += t.bytes;
  t.state = TextureState::kReady;
}

}  // namespace larecomp::citypack
