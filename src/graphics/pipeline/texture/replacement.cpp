/**
 * @file        graphics/pipeline/texture/replacement.cpp
 *
 * @brief       Texture dump and replacement pipeline implementation.
 *
 */
#include <rex/graphics/pipeline/texture/replacement.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

// stb_image — PNG/JPEG/etc. loader (implementation compiled exactly once here)
// STB_IMAGE_STATIC gives all symbols internal linkage, preventing duplicate
// symbol conflicts if the consuming application also vendors stb_image.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO  // we pass memory buffers directly
#include <stb_image.h>

namespace rex::graphics {

// ---------------------------------------------------------------------------
// DDS constants and structures
// ---------------------------------------------------------------------------

static constexpr uint32_t kDdsMagic        = 0x20534444u;  // "DDS "
static constexpr uint32_t kDdsdCaps        = 0x00000001u;
static constexpr uint32_t kDdsdHeight      = 0x00000002u;
static constexpr uint32_t kDdsdWidth       = 0x00000004u;
static constexpr uint32_t kDdsdPitch       = 0x00000008u;
static constexpr uint32_t kDdsdLinearSize  = 0x00080000u;
static constexpr uint32_t kDdsdPixelFormat = 0x00001000u;
static constexpr uint32_t kDdsPfRgb        = 0x00000040u;
static constexpr uint32_t kDdsPfAlphaPixels= 0x00000001u;
static constexpr uint32_t kDdsPfFourCC    = 0x00000004u;
static constexpr uint32_t kDdsCapsTexture  = 0x00001000u;

static constexpr uint32_t kFourCC_DXT1 = 0x31545844u;  // "DXT1"
static constexpr uint32_t kFourCC_DXT3 = 0x33545844u;  // "DXT3"
static constexpr uint32_t kFourCC_DXT5 = 0x35545844u;  // "DXT5"
static constexpr uint32_t kFourCC_ATI1 = 0x31495441u;  // "ATI1" (BC4 / DXN red)
static constexpr uint32_t kFourCC_ATI2 = 0x32495441u;  // "ATI2" (BC5 / DXN rg)

#pragma pack(push, 1)
struct DdsPixelFormat {
  uint32_t size = 32;
  uint32_t flags        = 0;
  uint32_t four_cc      = 0;
  uint32_t rgb_bit_count= 0;
  uint32_t r_bit_mask   = 0;
  uint32_t g_bit_mask   = 0;
  uint32_t b_bit_mask   = 0;
  uint32_t a_bit_mask   = 0;
};
struct DdsHeader {
  uint32_t      magic             = kDdsMagic;
  uint32_t      size              = 124;
  uint32_t      flags             = 0;
  uint32_t      height            = 0;
  uint32_t      width             = 0;
  uint32_t      pitch_or_linear   = 0;
  uint32_t      depth             = 0;
  uint32_t      mip_map_count     = 1;
  uint32_t      reserved1[11]     = {};
  DdsPixelFormat ddspf;
  uint32_t      caps              = kDdsCapsTexture;
  uint32_t      caps2             = 0;
  uint32_t      caps3             = 0;
  uint32_t      caps4             = 0;
  uint32_t      reserved2         = 0;
};
#pragma pack(pop)
static_assert(sizeof(DdsHeader) == 128);

// ---------------------------------------------------------------------------
// Internal helpers: tiled address decode (mirrors conversion.cpp)
// ---------------------------------------------------------------------------
static uint32_t TiledOffset2DRow(uint32_t y, uint32_t width, uint32_t log2_bpp) {
  uint32_t macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xFu) +
         ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
}

static uint32_t TiledOffset2DColumn(uint32_t x, uint32_t y, uint32_t log2_bpp,
                                    uint32_t base_offset) {
  uint32_t macro = (x / 32) << (log2_bpp + 7);
  uint32_t micro = (x & 7) << log2_bpp;
  uint32_t offset = base_offset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xFu));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0u) << 2) + (offset & 0x3Fu) +
         ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// Untile a 2-D block-based texture into a linear output buffer.
//   src              : tiled guest bytes
//   dst              : output buffer (row-major, no padding)
//   width_blocks     : visible width in blocks
//   height_blocks    : visible height in blocks
//   pitch_blocks     : row pitch in blocks (aligned to 32 for tiled)
//   bytes_per_block  : bytes per compressed block or texel
static void UntileBlocks(const uint8_t* src, uint8_t* dst,
                         uint32_t width_blocks, uint32_t height_blocks,
                         uint32_t pitch_blocks, uint32_t bytes_per_block) {
  // log2(bytes_per_block / 4) + extra bias matching Xenia's formula
  const uint32_t log2_bpp =
      (bytes_per_block / 4) + ((bytes_per_block / 2) >> (bytes_per_block / 4));

  const uint32_t out_row_bytes = width_blocks * bytes_per_block;

  for (uint32_t y = 0; y < height_blocks; ++y) {
    const uint32_t row_offset = TiledOffset2DRow(y, pitch_blocks, log2_bpp);
    for (uint32_t x = 0; x < width_blocks; ++x) {
      uint32_t src_offset = TiledOffset2DColumn(x, y, log2_bpp, row_offset);
      src_offset >>= log2_bpp;
      std::memcpy(dst + y * out_row_bytes + x * bytes_per_block,
                  src  + src_offset * bytes_per_block,
                  bytes_per_block);
    }
  }
}

// ---------------------------------------------------------------------------
// RGBA8 expansion helpers
// ---------------------------------------------------------------------------
// Each returns an RGBA8 pixel from a pointer into the (already endian-swapped)
// source data.

static uint32_t Expand5To8(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t Expand6To8(uint32_t v) { return (v << 2) | (v >> 4); }

// Convert one texel from the given format (already endian-corrected) to RGBA8.
// Returns false for formats that need the BC path (compressed blocks).
static bool TexelToRGBA8(const uint8_t* src, xenos::TextureFormat fmt, uint8_t out[4]) {
  using F = xenos::TextureFormat;
  switch (fmt) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_GAMMA_EDRAM:
      out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; out[3] = src[3];
      return true;
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      out[0] = out[1] = out[2] = src[0]; out[3] = 255;
      return true;
    case F::k_8_8:
      out[0] = src[0]; out[1] = src[1]; out[2] = 0; out[3] = 255;
      return true;
    case F::k_5_6_5: {
      uint16_t v; std::memcpy(&v, src, 2);
      out[0] = static_cast<uint8_t>(Expand5To8((v >> 11) & 0x1F));
      out[1] = static_cast<uint8_t>(Expand6To8((v >>  5) & 0x3F));
      out[2] = static_cast<uint8_t>(Expand5To8( v        & 0x1F));
      out[3] = 255;
      return true;
    }
    case F::k_1_5_5_5: {
      uint16_t v; std::memcpy(&v, src, 2);
      out[0] = static_cast<uint8_t>(Expand5To8((v >> 10) & 0x1F));
      out[1] = static_cast<uint8_t>(Expand5To8((v >>  5) & 0x1F));
      out[2] = static_cast<uint8_t>(Expand5To8( v        & 0x1F));
      out[3] = static_cast<uint8_t>(((v >> 15) & 1) ? 255 : 0);
      return true;
    }
    case F::k_4_4_4_4: {
      uint16_t v; std::memcpy(&v, src, 2);
      out[0] = static_cast<uint8_t>(((v >> 12) & 0xF) * 17);
      out[1] = static_cast<uint8_t>(((v >>  8) & 0xF) * 17);
      out[2] = static_cast<uint8_t>(((v >>  4) & 0xF) * 17);
      out[3] = static_cast<uint8_t>(( v        & 0xF) * 17);
      return true;
    }
    case F::k_2_10_10_10: {
      uint32_t v; std::memcpy(&v, src, 4);
      out[0] = static_cast<uint8_t>((v >> 22) & 0xFF);
      out[1] = static_cast<uint8_t>((v >> 12) & 0xFF);
      out[2] = static_cast<uint8_t>((v >>  2) & 0xFF);
      out[3] = static_cast<uint8_t>(((v & 3) * 85));
      return true;
    }
    default:
      // Unsupported / compressed — caller should use BC path or skip
      out[0] = out[1] = out[2] = out[3] = 0;
      return false;
  }
}

// ---------------------------------------------------------------------------
// DDS file writers
// ---------------------------------------------------------------------------

bool TextureReplacement::WriteDDS_RGBA8(const std::filesystem::path& path,
                                        uint32_t width, uint32_t height,
                                        const uint8_t* rgba8_rows,
                                        uint32_t row_pitch_bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return false;

  DdsHeader hdr;
  hdr.flags           = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat | kDdsdPitch;
  hdr.height          = height;
  hdr.width           = width;
  hdr.pitch_or_linear = width * 4;
  hdr.ddspf.flags     = kDdsPfRgb | kDdsPfAlphaPixels;
  hdr.ddspf.rgb_bit_count = 32;
  hdr.ddspf.r_bit_mask    = 0x000000FFu;
  hdr.ddspf.g_bit_mask    = 0x0000FF00u;
  hdr.ddspf.b_bit_mask    = 0x00FF0000u;
  hdr.ddspf.a_bit_mask    = 0xFF000000u;

  f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

  const uint32_t row_bytes = width * 4;
  for (uint32_t y = 0; y < height; ++y) {
    f.write(reinterpret_cast<const char*>(rgba8_rows + y * row_pitch_bytes), row_bytes);
  }
  return f.good();
}

bool TextureReplacement::WriteDDS_BC(const std::filesystem::path& path,
                                     uint32_t width, uint32_t height,
                                     const uint8_t* bc_blocks,
                                     uint32_t bytes_per_block,
                                     uint32_t fourcc) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return false;

  const uint32_t w_blocks = (width  + 3) / 4;
  const uint32_t h_blocks = (height + 3) / 4;
  const uint32_t linear_size = w_blocks * h_blocks * bytes_per_block;

  DdsHeader hdr;
  hdr.flags           = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat | kDdsdLinearSize;
  hdr.height          = height;
  hdr.width           = width;
  hdr.pitch_or_linear = linear_size;
  hdr.ddspf.flags     = kDdsPfFourCC;
  hdr.ddspf.four_cc   = fourcc;

  f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  f.write(reinterpret_cast<const char*>(bc_blocks), linear_size);
  return f.good();
}

// ---------------------------------------------------------------------------
// DDS reader (RGBA8 only — what tools export for replacements)
// ---------------------------------------------------------------------------
bool TextureReplacement::ReadDDS(const std::filesystem::path& path,
                                  TextureReplacementData& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return false;

  DdsHeader hdr{};
  f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  if (!f || hdr.magic != kDdsMagic || hdr.size != 124) return false;

  out.width      = hdr.width;
  out.height     = hdr.height;
  out.mip_levels = std::max(1u, hdr.mip_map_count);

  if (hdr.ddspf.rgb_bit_count != 32 ||
      hdr.ddspf.r_bit_mask    != 0x000000FFu ||
      hdr.ddspf.g_bit_mask    != 0x0000FF00u ||
      hdr.ddspf.b_bit_mask    != 0x00FF0000u) {
    REXLOG_WARN("TextureReplacement: {} has unsupported pixel format - must be RGBA8",
                path.filename().string());
    return false;
  }

  const size_t pixel_bytes = static_cast<size_t>(out.width) * out.height * 4;
  out.pixels.resize(pixel_bytes);
  f.read(reinterpret_cast<char*>(out.pixels.data()), static_cast<std::streamsize>(pixel_bytes));
  return f.good();
}

// ---------------------------------------------------------------------------
// TextureReplacement — construction / rescan
// ---------------------------------------------------------------------------

TextureReplacement::TextureReplacement(std::filesystem::path root)
    : root_(std::move(root)) {
  Rescan();
}

void TextureReplacement::Rescan() {
  replacements_.clear();
  pixel_cache_.clear();
  failed_cache_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(replace_dir(), ec)) return;

  for (auto& entry : std::filesystem::directory_iterator(replace_dir(), ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    auto& p = entry.path();
    const auto ext = p.extension();
    if (ext != ".dds" && ext != ".png") continue;

    const std::string stem = p.stem().string();
    if (stem.size() < 16) continue;

    uint64_t hash = 0;
    bool ok = true;
    for (int i = 0; i < 16; ++i) {
      char c = stem[i];
      uint64_t nibble = 0;
      if      (c >= '0' && c <= '9') nibble = static_cast<uint64_t>(c - '0');
      else if (c >= 'a' && c <= 'f') nibble = static_cast<uint64_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') nibble = static_cast<uint64_t>(c - 'A' + 10);
      else { ok = false; break; }
      hash = (hash << 4) | nibble;
    }
    if (!ok) continue;

    replacements_[hash] = p;
  }

  REXLOG_INFO("TextureReplacement: {} replacement(s) indexed from {}",
              replacements_.size(), replace_dir().string());
}

// ---------------------------------------------------------------------------
// Hash
// ---------------------------------------------------------------------------
uint64_t TextureReplacement::HashGuestData(const uint8_t* data, size_t size) {
  return XXH3_64bits(data, size);
}

// ---------------------------------------------------------------------------
// DumpTexture — untile + endian-swap + write DDS
// ---------------------------------------------------------------------------
void TextureReplacement::DumpTexture(uint64_t content_hash,
                                      uint32_t width, uint32_t height,
                                      uint32_t pitch_blocks,
                                      bool tiled,
                                      xenos::TextureFormat format,
                                      xenos::Endian endianness,
                                      const uint8_t* guest_bytes,
                                      uint32_t guest_size) const {
  using F = xenos::TextureFormat;

  const FormatInfo* fi = FormatInfo::Get(format);
  if (!fi) return;

  // Build filename: <hash16>_<w>x<h>_<format_name>.dds
  char name[128];
  std::snprintf(name, sizeof(name), "%016llx_%ux%u_%s.dds",
                static_cast<unsigned long long>(content_hash),
                width, height, fi->name);
  const auto dest = dump_dir() / name;

  // Only write once per unique texture to avoid hammering the disk.
  if (std::filesystem::exists(dest)) return;

  const uint32_t bpb          = fi->bytes_per_block();
  const uint32_t w_blocks     = (width  + fi->block_width  - 1) / fi->block_width;
  const uint32_t h_blocks     = (height + fi->block_height - 1) / fi->block_height;
  // pitch_blocks is in units of 32 texels, convert to block units
  const uint32_t pitch_b32    = pitch_blocks * 32;            // pitch in texels
  const uint32_t pitch_blk    = (pitch_b32 + fi->block_width - 1) / fi->block_width;

  // Step 1 — allocate a linear staging buffer and untile (or copy linear)
  const uint32_t linear_bytes = w_blocks * h_blocks * bpb;
  std::vector<uint8_t> linear(linear_bytes);

  if (tiled) {
    UntileBlocks(guest_bytes, linear.data(), w_blocks, h_blocks, pitch_blk, bpb);
  } else {
    // Linear: rows are already in order but may have pitch padding — copy
    // only the visible region.
    const uint32_t src_row_bytes = pitch_blk * bpb;
    const uint32_t dst_row_bytes = w_blocks  * bpb;
    for (uint32_t y = 0; y < h_blocks; ++y) {
      const uint32_t src_off = y * src_row_bytes;
      if (src_off + dst_row_bytes > guest_size) break;
      std::memcpy(linear.data() + y * dst_row_bytes,
                  guest_bytes   + src_off,
                  dst_row_bytes);
    }
  }

  // Step 2 — endian-swap the staging buffer in-place using CopySwapBlock
  if (endianness != xenos::Endian::kNone) {
    texture_conversion::CopySwapBlock(endianness, linear.data(), linear.data(), linear_bytes);
  }

  // Step 3 — write to DDS
  if (fi->type == FormatType::kCompressed) {
    // Determine DDS FOURCC
    uint32_t fourcc = 0;
    switch (format) {
      case F::k_DXT1:
      case F::k_DXT1_AS_16_16_16_16: fourcc = kFourCC_DXT1; break;
      case F::k_DXT2_3:
      case F::k_DXT2_3_AS_16_16_16_16: fourcc = kFourCC_DXT3; break;
      case F::k_DXT4_5:
      case F::k_DXT4_5_AS_16_16_16_16: fourcc = kFourCC_DXT5; break;
      case F::k_DXN:                  fourcc = kFourCC_ATI2; break;
      case F::k_DXT5A:                fourcc = kFourCC_ATI1; break;
      // DXT3A variants: treat as DXT3 (same block layout)
      case F::k_DXT3A:
      case F::k_DXT3A_AS_1_1_1_1:    fourcc = kFourCC_DXT3; break;
      default:                        fourcc = kFourCC_DXT5; break;
    }

    if (!WriteDDS_BC(dest, width, height, linear.data(), bpb, fourcc)) {
      REXLOG_WARN("TextureReplacement: failed to write BC dump {}", dest.string());
    } else {
      REXLOG_DEBUG("TextureReplacement: dumped BC  {}", dest.filename().string());
    }
  } else {
    // Uncompressed — expand each texel to RGBA8
    const uint32_t out_row_bytes = w_blocks * 4;  // w_blocks == width for uncompressed
    std::vector<uint8_t> rgba(static_cast<size_t>(w_blocks) * h_blocks * 4);

    bool ok = true;
    for (uint32_t y = 0; y < h_blocks && ok; ++y) {
      for (uint32_t x = 0; x < w_blocks; ++x) {
        const uint8_t* src = linear.data() + (y * w_blocks + x) * bpb;
        uint8_t* dst = rgba.data() + y * out_row_bytes + x * 4;
        if (!TexelToRGBA8(src, format, dst)) {
          // Unsupported format — write raw bytes zero-padded to RGBA8 as
          // a best-effort so at least something useful shows up.
          dst[0] = bpb > 0 ? src[0] : 0;
          dst[1] = bpb > 1 ? src[1] : 0;
          dst[2] = bpb > 2 ? src[2] : 0;
          dst[3] = bpb > 3 ? src[3] : 255;
        }
      }
    }

    if (!WriteDDS_RGBA8(dest, width, height, rgba.data(), out_row_bytes)) {
      REXLOG_WARN("TextureReplacement: failed to write RGBA8 dump {}", dest.string());
    } else {
      REXLOG_DEBUG("TextureReplacement: dumped RGBA8 {}", dest.filename().string());
    }
  }
}

// ---------------------------------------------------------------------------
// PNG reader (RGBA8 — via stb_image)
// ---------------------------------------------------------------------------
bool TextureReplacement::ReadPNG(const std::filesystem::path& path,
                                  TextureReplacementData& out) {
  // Read the whole file into memory first so we can use stbi_load_from_memory
  // (avoids any stdio FILE* locale issues on Windows).
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return false;

  const auto file_size = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<uint8_t> buf(file_size);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(file_size));
  if (!f) return false;

  int w = 0, h = 0, channels = 0;
  uint8_t* data = stbi_load_from_memory(buf.data(),
                                         static_cast<int>(file_size),
                                         &w, &h, &channels, 4 /*RGBA*/);
  if (!data) {
    REXLOG_WARN("TextureReplacement: stb_image failed to load {}: {}",
                path.filename().string(), stbi_failure_reason());
    return false;
  }

  out.width      = static_cast<uint32_t>(w);
  out.height     = static_cast<uint32_t>(h);
  out.mip_levels = 1;
  out.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  return true;
}

// ---------------------------------------------------------------------------
// FindReplacement
// ---------------------------------------------------------------------------
const TextureReplacementData* TextureReplacement::FindReplacement(uint64_t content_hash) const {
  // Already cached (success)?
  {
    auto it = pixel_cache_.find(content_hash);
    if (it != pixel_cache_.end()) {
      return &it->second;
    }
  }

  // Known failure — don't retry.
  if (failed_cache_.count(content_hash)) return nullptr;

  auto it = replacements_.find(content_hash);
  if (it == replacements_.end()) {
    failed_cache_.insert(content_hash);
    return nullptr;
  }

  const auto& p = it->second;
  const auto ext = p.extension();
  TextureReplacementData loaded;
  bool ok = false;
  if (ext == ".png") {
    ok = ReadPNG(p, loaded);
  } else {
    ok = ReadDDS(p, loaded);
  }

  if (!ok) {
    REXLOG_WARN("TextureReplacement: failed to load {}", p.filename().string());
    failed_cache_.insert(content_hash);
    return nullptr;
  }

  auto [ins, _] = pixel_cache_.emplace(content_hash, std::move(loaded));
  return &ins->second;
}

}  // namespace rex::graphics
