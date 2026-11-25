#include "citypack/city_zstd.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <zstd.h>

namespace larecomp::citypack {

namespace {

struct CctxDeleter {
  void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct DctxDeleter {
  void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

// One context a thread: a worker compresses hundreds of pieces and the library keeps its tables between them.
ZSTD_CCtx* ThreadCompressContext() {
  thread_local std::unique_ptr<ZSTD_CCtx, CctxDeleter> context(ZSTD_createCCtx());
  return context.get();
}
ZSTD_DCtx* ThreadDecompressContext() {
  thread_local std::unique_ptr<ZSTD_DCtx, DctxDeleter> context(ZSTD_createDCtx());
  return context.get();
}

// A block format's bytes by plane. `Size` bytes of every `Block` bytes go to one plane; the planes follow each other (all of the first, then all of the second ...).
template <size_t Block, size_t Size>
void SplitPlane(const uint8_t* in, size_t blocks, uint8_t* out) {
  for (size_t b = 0; b < blocks; ++b) std::memcpy(out + b * Size, in + b * Block, Size);
}
template <size_t Block, size_t Size>
void MergePlane(const uint8_t* plane, size_t blocks, uint8_t* out) {
  for (size_t b = 0; b < blocks; ++b) std::memcpy(out + b * Block, plane + b * Size, Size);
}

}  // namespace

bool ZstdCompress(const uint8_t* src, size_t size, int level, std::vector<uint8_t>* out) {
  ZSTD_CCtx* context = ThreadCompressContext();
  if (context == nullptr) return false;
  level = std::clamp(level, 1, kZstdLevelMax);
  if (ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)) || ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 1))) {
    return false;
  }
  out->resize(ZSTD_compressBound(size));
  const size_t written = ZSTD_compress2(context, out->data(), out->size(), src, size);
  if (ZSTD_isError(written)) {
    out->clear();
    return false;
  }
  out->resize(written);
  return true;
}

bool ZstdDecompress(const uint8_t* src, size_t size, uint8_t* dest, size_t dest_size, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (size == 0) return fail("a compressed piece is empty");
  const unsigned long long content = ZSTD_getFrameContentSize(src, size);
  if (content == ZSTD_CONTENTSIZE_ERROR) return fail("a compressed piece is not a zstd frame");
  if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != dest_size) return fail("a compressed piece's size is not the one its table says");
  ZSTD_DCtx* context = ThreadDecompressContext();
  if (context == nullptr) return fail("no memory for the decompressor");
  const size_t got = ZSTD_decompressDCtx(context, dest, dest_size, src, size);
  if (ZSTD_isError(got)) return fail(std::string("a compressed piece is damaged (") + ZSTD_getErrorName(got) + ")");
  if (got != dest_size) return fail("a compressed piece is shorter than its table says");
  return true;
}

bool ZstdDecompressSelfSized(const uint8_t* src, size_t size, size_t max_size, std::vector<uint8_t>* out, std::string* error) {
  if (size == 0) {
    if (error) *error = "a compressed piece is empty";
    return false;
  }
  const unsigned long long content = ZSTD_getFrameContentSize(src, size);
  if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN || content > max_size) {
    if (error) *error = "a compressed piece does not say a size this build accepts";
    return false;
  }
  out->resize(size_t(content));
  return ZstdDecompress(src, size, out->data(), out->size(), error);
}

void BcSplitPlanes(HitTextureFormat format, const uint8_t* in, size_t bytes, uint8_t* out) {
  if (bytes == 0) return;  // (null pointers of an empty vector: no pointer arithmetic on them)
  switch (format) {
    case HitTextureFormat::kBc1: {  // endpoints (two RGB565), then the 2 bit indices
      const size_t blocks = bytes / 8;
      SplitPlane<8, 4>(in, blocks, out);
      SplitPlane<8, 4>(in + 4, blocks, out + blocks * 4);
      return;
    }
    case HitTextureFormat::kBc2: {  // the explicit alpha, the colour endpoints, the colour indices
      const size_t blocks = bytes / 16;
      SplitPlane<16, 8>(in, blocks, out);
      SplitPlane<16, 4>(in + 8, blocks, out + blocks * 8);
      SplitPlane<16, 4>(in + 12, blocks, out + blocks * 12);
      return;
    }
    case HitTextureFormat::kBc3: {  // the alpha endpoints, the alpha indices, the colour endpoints, the colour indices
      const size_t blocks = bytes / 16;
      SplitPlane<16, 2>(in, blocks, out);
      SplitPlane<16, 6>(in + 2, blocks, out + blocks * 2);
      SplitPlane<16, 4>(in + 8, blocks, out + blocks * 8);
      SplitPlane<16, 4>(in + 12, blocks, out + blocks * 12);
      return;
    }
    default:
      std::memcpy(out, in, bytes);
      return;
  }
}

void BcMergePlanes(HitTextureFormat format, const uint8_t* in, size_t bytes, uint8_t* out) {
  if (bytes == 0) return;
  switch (format) {
    case HitTextureFormat::kBc1: {
      const size_t blocks = bytes / 8;
      MergePlane<8, 4>(in, blocks, out);
      MergePlane<8, 4>(in + blocks * 4, blocks, out + 4);
      return;
    }
    case HitTextureFormat::kBc2: {
      const size_t blocks = bytes / 16;
      MergePlane<16, 8>(in, blocks, out);
      MergePlane<16, 4>(in + blocks * 8, blocks, out + 8);
      MergePlane<16, 4>(in + blocks * 12, blocks, out + 12);
      return;
    }
    case HitTextureFormat::kBc3: {
      const size_t blocks = bytes / 16;
      MergePlane<16, 2>(in, blocks, out);
      MergePlane<16, 6>(in + blocks * 2, blocks, out + 2);
      MergePlane<16, 4>(in + blocks * 8, blocks, out + 8);
      MergePlane<16, 4>(in + blocks * 12, blocks, out + 12);
      return;
    }
    default:
      std::memcpy(out, in, bytes);
      return;
  }
}

bool CompressTextureChain(const HitTexture& texture, const uint8_t* chain, int level, std::vector<uint8_t>* stored) {
  const size_t size = size_t(texture.ChainBytes(0));
  if (size == 0 || size != texture.data_size) return false;
  std::vector<uint8_t> planed(size);
  size_t offset = 0;
  for (uint32_t m = 0; m < texture.mip_count; ++m) {
    const size_t bytes = size_t(HitMipBytes(texture.format, uint32_t(texture.width) >> m, uint32_t(texture.height) >> m));
    BcSplitPlanes(texture.format, chain + offset, bytes, planed.data() + offset);
    offset += bytes;
  }
  std::vector<uint8_t> frame;
  if (!ZstdCompress(planed.data(), planed.size(), level, &frame)) return false;
  if (frame.size() < size) {
    *stored = std::move(frame);
  } else {
    stored->assign(chain, chain + size);  // not smaller: the chain as it is (stored size == data size)
  }
  return true;
}

bool UnpackTextureChain(const HitTexture& texture, const uint8_t* stored, size_t stored_size, uint32_t first_mip, std::vector<uint8_t>* out, std::vector<uint8_t>* scratch,
                        std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  const size_t size = size_t(texture.ChainBytes(0));
  if (size == 0 || size != texture.data_size) return fail("a texture record's size does not match its levels");
  if (first_mip >= texture.mip_count) return fail("no such mip level");
  const size_t tail_offset = size_t(texture.MipOffset(first_mip));
  if (stored_size == size) {  // the chain as it is
    out->assign(stored + tail_offset, stored + size);
    return true;
  }
  if (stored_size == 0 || stored_size > size) return fail("a texture's stored size does not fit it");
  scratch->resize(size);
  if (!ZstdDecompress(stored, stored_size, scratch->data(), size, error)) return false;
  out->resize(size - tail_offset);
  size_t offset = tail_offset;
  for (uint32_t m = first_mip; m < texture.mip_count; ++m) {
    const size_t bytes = size_t(HitMipBytes(texture.format, uint32_t(texture.width) >> m, uint32_t(texture.height) >> m));
    BcMergePlanes(texture.format, scratch->data() + offset, bytes, out->data() + (offset - tail_offset));
    offset += bytes;
  }
  return true;
}

}  // namespace larecomp::citypack
