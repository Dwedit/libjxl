// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef JXL_IMAGE_BUNDLE_H_
#define JXL_IMAGE_BUNDLE_H_

// The main image or frame consists of a bundle of associated images.

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "jxl/aux_out_fwd.h"
#include "jxl/base/compiler_specific.h"
#include "jxl/base/data_parallel.h"
#include "jxl/base/status.h"
#include "jxl/color_encoding.h"
#include "jxl/common.h"
#include "jxl/dec_bit_reader.h"
#include "jxl/dec_xyb.h"
#include "jxl/enc_bit_writer.h"
#include "jxl/field_encodings.h"
#include "jxl/frame_header.h"
#include "jxl/image.h"
#include "jxl/opsin_params.h"
#include "jxl/quantizer.h"

namespace jxl {

// EXIF orientation of the image. This field overrides any field present in
// actual EXIF metadata. The value tells which transformation the decoder must
// apply after decoding to display the image with the correct orientation.
enum class Orientation : uint32_t {
  // Values 1..8 match the EXIF definitions.
  kIdentity = 1,
  kFlipHorizontal,
  kRotate180,
  kFlipVertical,
  kTranspose,
  kRotate90,
  kAntiTranspose,
  kRotate270,
};
// Don't need an EnumBits because Orientation is not read via Enum().

enum class ExtraChannel : uint32_t {
  // First two enumerators (most common) are cheaper to encode
  kAlpha,
  kDepth,

  kSpotColor,
  kSelectionMask,
  kBlack,  // for CMYK
  kCFA,    // Bayer channel
  kThermal,
  kReserved0,
  kReserved1,
  kReserved2,
  kReserved3,
  kReserved4,
  kReserved5,
  kReserved6,
  kReserved7,
  kUnknown,  // disambiguated via name string, raise warning if unsupported
  kOptional  // like kUnknown but can silently be ignored
};
static inline const char* EnumName(ExtraChannel /*unused*/) {
  return "ExtraChannel";
}
static inline constexpr uint64_t EnumBits(ExtraChannel /*unused*/) {
  using EC = ExtraChannel;
  return MakeBit(EC::kAlpha) | MakeBit(EC::kDepth) | MakeBit(EC::kSpotColor) |
         MakeBit(EC::kSelectionMask) | MakeBit(EC::kBlack) | MakeBit(EC::kCFA) |
         MakeBit(EC::kUnknown) | MakeBit(EC::kOptional);
}

// Used in ImageMetadata and ExtraChannelInfo.
struct BitDepth {
  BitDepth();
  static const char* Name() { return "BitDepth"; }

  template <class Visitor>
  Status VisitFields(Visitor* JXL_RESTRICT visitor) {
    visitor->Bool(false, &floating_point_sample);
    // The same fields (bits_per_sample and exponent_bits_per_sample) are read
    // in a different way depending on floating_point_sample's value. It's still
    // default-initialized correctly so using visitor->Conditional is not
    // required.
    if (!floating_point_sample) {
      visitor->U32(Val(8), Val(10), Val(12), BitsOffset(5, 1), 8,
                   &bits_per_sample);
      exponent_bits_per_sample = 0;
    } else {
      visitor->U32(Val(32), Val(16), Val(24), Bits(5), 32, &bits_per_sample);
      // The encoded value is exponent_bits_per_sample - 1, encoded in 3 bits
      // so the value can be in range [1, 8].
      const uint32_t offset = 1;
      exponent_bits_per_sample -= offset;
      visitor->Bits(3, 8 - offset, &exponent_bits_per_sample);
      exponent_bits_per_sample += offset;
    }

    // Error-checking for floating point ranges.
    if (floating_point_sample) {
      if (exponent_bits_per_sample < 2 || exponent_bits_per_sample > 8) {
        return JXL_FAILURE("Invalid exponent_bits_per_sample");
      }
      int mantissa_bits =
          static_cast<int>(bits_per_sample) - exponent_bits_per_sample - 1;
      if (mantissa_bits < 2 || mantissa_bits > 23) {
        return JXL_FAILURE("Invalid bits_per_sample");
      }
    }
    return true;
  }

  // Whether the original (uncompressed) samples are floating point or
  // unsigned integer.
  bool floating_point_sample;

  // Bit depth of the original (uncompressed) image samples. Must be in the
  // range [1, 32].
  uint32_t bits_per_sample;

  // Floating point exponent bits of the original (uncompressed) image samples,
  // only used if floating_point_sample is true.
  // If used, the samples are floating point with:
  // - 1 sign bit
  // - exponent_bits_per_sample exponent bits
  // - (bits_per_sample - exponent_bits_per_sample - 1) mantissa bits
  // If used, exponent_bits_per_sample must be in the range
  // [2, 8] and amount of mantissa bits must be in the range [2, 23].
  // NOTE: exponent_bits_per_sample is 8 for single precision binary32
  // point, 5 for half precision binary16, 7 for fp24.
  uint32_t exponent_bits_per_sample;
};

// Describes one extra channel.
struct ExtraChannelInfo {
  ExtraChannelInfo();
  static const char* Name() { return "ExtraChannelInfo"; }

  template <class Visitor>
  Status VisitFields(Visitor* JXL_RESTRICT visitor) {
    if (visitor->AllDefault(*this, &all_default)) {
      // Overwrite all serialized fields, but not any nonserialized_*.
      visitor->SetDefault(this);
      return true;
    }

    // General
    JXL_RETURN_IF_ERROR(visitor->Enum(ExtraChannel::kAlpha, &type));
    visitor->Bool(false, &add_to_prev_frame);  // ? kAdd : kReplace

    JXL_RETURN_IF_ERROR(visitor->VisitNested(&bit_depth));

    visitor->U32(Val(0), Val(3), Val(4), BitsOffset(3, 1), 0, &dim_shift);
    if ((1U << dim_shift) > kGroupDim) {
      return JXL_FAILURE("dim_shift %u too large", dim_shift);
    }

    uint32_t name_length = name.length();
    // Allows name lengths up to 1071 bytes
    visitor->U32(Val(0), Bits(4), BitsOffset(5, 16), BitsOffset(10, 48), 0,
                 &name_length);
    if (visitor->IsReading()) {
      name.resize(name_length);
    }
    for (size_t i = 0; i < name_length; i++) {
      uint32_t c = name[i];
      visitor->Bits(8, 0, &c);
      name[i] = c;
    }

    // Conditional
    if (visitor->Conditional(type == ExtraChannel::kAlpha)) {
      visitor->Bool(false, &alpha_associated);
    }
    if (visitor->Conditional(type == ExtraChannel::kSpotColor)) {
      for (float& c : spot_color) {
        visitor->F16(0, &c);
      }
    }
    if (visitor->Conditional(type == ExtraChannel::kCFA)) {
      visitor->U32(Val(1), Bits(2), BitsOffset(4, 3), BitsOffset(8, 19), 1,
                   &cfa_channel);
    }
    return true;
  }

  size_t Size(size_t size) const {
    const size_t mask = (1u << dim_shift) - 1;
    return (size + mask) >> dim_shift;
  }

  mutable bool all_default;

  ExtraChannel type;
  bool add_to_prev_frame;
  BitDepth bit_depth;
  uint32_t dim_shift;  // downsampled by 2^dim_shift on each axis

  std::string name;  // UTF-8

  // Conditional:
  bool alpha_associated;  // i.e. premultiplied
  float spot_color[4];    // spot color in linear RGBA
  uint32_t cfa_channel;
};

struct OpsinInverseMatrix {
  OpsinInverseMatrix();
  static const char* Name() { return "OpsinInverseMatrix"; }

  template <class Visitor>
  Status VisitFields(Visitor* JXL_RESTRICT visitor) {
    if (visitor->AllDefault(*this, &all_default)) {
      // Overwrite all serialized fields, but not any nonserialized_*.
      visitor->SetDefault(this);
      return true;
    }
    for (int i = 0; i < 9; ++i) {
      visitor->F16(DefaultInverseOpsinAbsorbanceMatrix()[i],
                   &inverse_matrix[i]);
    }
    for (int i = 0; i < 3; ++i) {
      visitor->F16(kNegOpsinAbsorbanceBiasRGB[i], &opsin_biases[i]);
    }
    for (int i = 0; i < 4; ++i) {
      visitor->F16(kDefaultQuantBias[i], &quant_biases[i]);
    }
    return true;
  }

  OpsinParams ToOpsinParams() const {
    OpsinParams opsin_params;
    InitSIMDInverseMatrix(inverse_matrix, opsin_params.inverse_opsin_matrix);
    std::copy(std::begin(opsin_biases), std::end(opsin_biases),
              opsin_params.opsin_biases);
    for (int i = 0; i < 3; ++i) {
      opsin_params.opsin_biases_cbrt[i] =
          std::cbrt(opsin_params.opsin_biases[i]);
    }
    opsin_params.opsin_biases_cbrt[3] = opsin_params.opsin_biases[3] = 255;
    std::copy(std::begin(quant_biases), std::end(quant_biases),
              opsin_params.quant_biases);
    return opsin_params;
  }

  mutable bool all_default;

  float inverse_matrix[9];
  float opsin_biases[3];
  float quant_biases[4];
};

// Information useful for mapping HDR images to lower dynamic range displays.
struct ToneMapping {
  ToneMapping();
  static const char* Name() { return "ToneMapping"; }

  template <class Visitor>
  Status VisitFields(Visitor* JXL_RESTRICT visitor) {
    if (visitor->AllDefault(*this, &all_default)) {
      // Overwrite all serialized fields, but not any nonserialized_*.
      visitor->SetDefault(this);
      return true;
    }

    visitor->F16(kDefaultIntensityTarget, &intensity_target);
    if (intensity_target <= 0.f) {
      return JXL_FAILURE("invalid intensity target");
    }

    visitor->F16(0.0f, &min_nits);
    if (min_nits < 0.f || min_nits > intensity_target) {
      return JXL_FAILURE("invalid min %f vs max %f", min_nits,
                         intensity_target);
    }

    visitor->Bool(false, &relative_to_max_display);

    visitor->F16(0.0f, &linear_below);
    if (linear_below < 0 || (relative_to_max_display && linear_below > 1.0f)) {
      return JXL_FAILURE("invalid linear_below %f (%s)", linear_below,
                         relative_to_max_display ? "relative" : "absolute");
    }

    return true;
  }

  mutable bool all_default;

  // Upper bound on the intensity level present in the image. For unsigned
  // integer pixel encodings, this is the brightness of the largest
  // representable value. The image does not necessarily contain a pixel
  // actually this bright. An encoder is allowed to set 255 for SDR images
  // without computing a histogram.
  float intensity_target;  // [nits]

  // Lower bound on the intensity level present in the image. This may be
  // loose, i.e. lower than the actual darkest pixel. When tone mapping, a
  // decoder will map [min_nits, intensity_target] to the display range.
  float min_nits;

  bool relative_to_max_display;  // see below
  // The tone mapping will leave unchanged (linear mapping) any pixels whose
  // brightness is strictly below this. The interpretation depends on
  // relative_to_max_display. If true, this is a ratio [0, 1] of the maximum
  // display brightness [nits], otherwise an absolute brightness [nits].
  float linear_below;
};

// Less frequently changed fields, grouped into a separate bundle so they do not
// need to be signaled when some ImageMetadata fields are non-default.
struct ImageMetadata2 {
  ImageMetadata2();
  static const char* Name() { return "ImageMetadata2"; }

  template <class Visitor>
  Status VisitFields(Visitor* JXL_RESTRICT visitor) {
    if (visitor->AllDefault(*this, &all_default)) {
      // Overwrite all serialized fields, but not any nonserialized_*.
      visitor->SetDefault(this);
      return true;
    }

    visitor->Bool(false, &have_preview);
    visitor->Bool(false, &have_animation);

    visitor->Bits(3, 0, &orientation_minus_1);
    // (No need for bounds checking because we read exactly 3 bits)

    num_extra_channels = extra_channel_info.size();
    visitor->U32(Val(0), Bits(4), BitsOffset(8, 16), BitsOffset(12, 1), 0,
                 &num_extra_channels);

    // Treat as if only the fields up to num_extra_channels exist.
    if (visitor->IsReading() && nonserialized_only_parse_basic_info) {
      return true;
    }

    if (visitor->Conditional(num_extra_channels != 0)) {
      if (visitor->IsReading()) {
        extra_channel_info.resize(num_extra_channels);
      }
      for (ExtraChannelInfo& eci : extra_channel_info) {
        JXL_RETURN_IF_ERROR(visitor->VisitNested(&eci));
      }
    }

    JXL_RETURN_IF_ERROR(visitor->VisitNested(&opsin_inverse_matrix));

    visitor->BeginExtensions(&extensions);
    // Extensions: in chronological order of being added to the format.
    return visitor->EndExtensions();
  }

  // Returns first ExtraChannelInfo of the given type, or nullptr if none.
  const ExtraChannelInfo* Find(ExtraChannel type) const {
    for (const ExtraChannelInfo& eci : extra_channel_info) {
      if (eci.type == type) return &eci;
    }
    return nullptr;
  }

  mutable bool all_default;

  bool have_preview;
  bool have_animation;

  uint32_t orientation_minus_1;

  // When reading: deserialized. When writing: automatically set from vector.
  uint32_t num_extra_channels;
  std::vector<ExtraChannelInfo> extra_channel_info;

  OpsinInverseMatrix opsin_inverse_matrix;

  uint64_t extensions;

  // Option to stop parsing after extra_channel_bits, and treat as if the later
  // fields do not participate. Use to parse only basic image information
  // excluding the final larger or variable sized data.
  bool nonserialized_only_parse_basic_info = false;
};

// Properties of the original image bundle. This enables Encode(Decode()) to
// re-create an equivalent image without user input.
struct ImageMetadata {
  ImageMetadata();
  static const char* Name() { return "ImageMetadata"; }

  template <class Visitor>
  Status VisitFields(Visitor* JXL_RESTRICT visitor) {
    if (visitor->AllDefault(*this, &all_default)) {
      // Overwrite all serialized fields, but not any nonserialized_*.
      visitor->SetDefault(this);
      return true;
    }

    JXL_RETURN_IF_ERROR(visitor->VisitNested(&bit_depth));

    JXL_RETURN_IF_ERROR(visitor->VisitNested(&color_encoding));
    visitor->U32(Val(0), Val(8), Val(16), Bits(4), 0, &alpha_bits);

    JXL_RETURN_IF_ERROR(visitor->VisitNested(&tone_mapping_));
    JXL_RETURN_IF_ERROR(visitor->VisitNested(&m2));

    return true;
  }

  bool HasAlpha() const { return alpha_bits != 0; }

  // Sets the original bit depth fields to indicate unsigned integer of the
  // given bit depth.
  void SetUintSamples(uint32_t bits) {
    bit_depth.bits_per_sample = bits;
    bit_depth.exponent_bits_per_sample = 0;
    bit_depth.floating_point_sample = false;
  }
  // Sets the original bit depth fields to indicate single precision floating
  // point.
  void SetFloat32Samples() {
    bit_depth.bits_per_sample = 32;
    bit_depth.exponent_bits_per_sample = 8;
    bit_depth.floating_point_sample = true;
  }

  void SetIntensityTarget(float intensity_target) {
    tone_mapping_.intensity_target = intensity_target;
  }
  float IntensityTarget() const { return tone_mapping_.intensity_target; }

  mutable bool all_default;

  ImageMetadata2 m2;  // often default

  BitDepth bit_depth;

  ColorEncoding color_encoding;

  // Bit depth of the JPEG XL compressed alpha channel.
  // 0 if no alpha channel present.
  // TODO(janwas): move to ExtraChannelInfo
  uint32_t alpha_bits;

 private:
  ToneMapping tone_mapping_;
};

Status ReadImageMetadata(BitReader* JXL_RESTRICT reader,
                         ImageMetadata* JXL_RESTRICT metadata);

Status WriteImageMetadata(const ImageMetadata& metadata,
                          BitWriter* JXL_RESTRICT writer, size_t layer,
                          AuxOut* aux_out);

// A bundle of color/alpha/depth/plane images.
class ImageBundle {
 public:
  // Uninitialized state for use as output parameter.
  ImageBundle() : metadata_(nullptr) {}
  // Caller is responsible for setting metadata before calling Set*.
  explicit ImageBundle(ImageMetadata* metadata) : metadata_(metadata) {}

  // Move-only (allows storing in std::vector).
  ImageBundle(ImageBundle&&) = default;
  ImageBundle& operator=(ImageBundle&&) = default;

  ImageBundle Copy() const {
    ImageBundle copy(metadata_);
    copy.color_ = CopyImage(color_);
    copy.c_current_ = c_current_;
    copy.alpha_ = CopyImage(alpha_);
    copy.extra_channels_.reserve(extra_channels_.size());
    for (const ImageU& plane : extra_channels_) {
      copy.extra_channels_.emplace_back(CopyImage(plane));
    }

    copy.is_jpeg = is_jpeg;
    copy.jpeg_quant_table = jpeg_quant_table;
    copy.jpeg_xsize = jpeg_xsize;
    copy.jpeg_ysize = jpeg_ysize;
    copy.color_transform = color_transform;
    copy.chroma_subsampling = chroma_subsampling;

    return copy;
  }

  // -- SIZE

  size_t xsize() const {
    if (jpeg_xsize != 0) return jpeg_xsize;
    if (color_.xsize() != 0) return color_.xsize();
    return extra_channels_.empty() ? 0 : extra_channels_[0].xsize();
  }
  size_t ysize() const {
    if (jpeg_ysize != 0) return jpeg_ysize;
    if (color_.ysize() != 0) return color_.ysize();
    return extra_channels_.empty() ? 0 : extra_channels_[0].ysize();
  }
  void ShrinkTo(size_t xsize, size_t ysize);

  // -- COLOR

  // Whether color() is valid/usable. Returns true in most cases. Even images
  // with spot colors (one example of when !planes().empty()) typically have a
  // part that can be converted to RGB.
  bool HasColor() const { return color_.xsize() != 0; }

  // For resetting the size when switching from a reference to main frame.
  void RemoveColor() { color_ = Image3F(); }

  // Do not use if !HasColor().
  const Image3F& color() const {
    // If this fails, Set* was not called - perhaps because decoding failed?
    JXL_DASSERT(HasColor());
    return color_;
  }

  // Do not use if !HasColor().
  Image3F* MutableColor() {
    JXL_DASSERT(HasColor());
    return &color_;
  }
  Image3F* MutableColor() const {
    JXL_DASSERT(HasColor());
    return const_cast<Image3F*>(&color_);
  }

  // If c_current.IsGray(), all planes must be identical. NOTE: c_current is
  // independent of metadata()->color_encoding, which is the original, whereas
  // a decoder might return pixels in a different c_current.
  void SetFromImage(Image3F&& color, const ColorEncoding& c_current);

  // Sets image data from 8-bit sRGB pixel array in bytes.
  // Amount of input bytes per pixel must be:
  // (is_gray ? 1 : 3) + (has_alpha ? 1 : 0)
  Status SetFromSRGB(size_t xsize, size_t ysize, bool is_gray, bool has_alpha,
                     bool alpha_is_premultiplied, const uint8_t* pixels,
                     const uint8_t* end, ThreadPool* pool = nullptr);

  // Sets image data from 16-bit sRGB data.
  // Amount of input uint16_t's per pixel must be:
  // (is_gray ? 1 : 3) + (has_alpha ? 1 : 0)
  Status SetFromSRGB(size_t xsize, size_t ysize, bool is_gray, bool has_alpha,
                     bool alpha_is_premultiplied, const uint16_t* pixels,
                     const uint16_t* end, ThreadPool* pool = nullptr);

  // Sets image data from sRGB pixel array in bytes.
  // This low-level function supports both 8-bit and 16-bit data in bytes to
  // provide efficient access to arbitrary byte order.
  // Amount of input bytes per pixel must be:
  // ((is_gray ? 1 : 3) + (has_alpha ? 1 : 0)) * (is_16bit ? 2 : 1)
  // The ordering of the channels is interleaved RGBA or gray+alpha in that
  // order.
  // The 16-bit byte order is given by big_endian, and this has no effect when
  // is_16bit is false.
  Status SetFromSRGB(size_t xsize, size_t ysize, bool is_gray, bool has_alpha,
                     bool alpha_is_premultiplied, bool is_16bit,
                     bool big_endian, const uint8_t* pixels, const uint8_t* end,
                     ThreadPool* pool = nullptr);

  // -- COLOR ENCODING

  const ColorEncoding& c_current() const { return c_current_; }

  // Returns whether the color image has identical planes. Once established by
  // Set*, remains unchanged until a subsequent Set* or TransformTo.
  bool IsGray() const { return c_current_.IsGray(); }

  bool IsSRGB() const { return c_current_.IsSRGB(); }
  bool IsLinearSRGB() const {
    return c_current_.white_point == WhitePoint::kD65 &&
           c_current_.primaries == Primaries::kSRGB && c_current_.tf.IsLinear();
  }

  // Transforms color to c_desired and sets c_current to c_desired. Alpha and
  // metadata remains unchanged.
  Status TransformTo(const ColorEncoding& c_desired,
                     ThreadPool* pool = nullptr);

  // Copies this:rect, converts to c_desired, and allocates+fills out.
  Status CopyTo(const Rect& rect, const ColorEncoding& c_desired, Image3B* out,
                ThreadPool* pool = nullptr) const;
  Status CopyTo(const Rect& rect, const ColorEncoding& c_desired, Image3U* out,
                ThreadPool* pool = nullptr) const;
  Status CopyTo(const Rect& rect, const ColorEncoding& c_desired, Image3F* out,
                ThreadPool* pool = nullptr) const;
  Status CopyToSRGB(const Rect& rect, Image3B* out,
                    ThreadPool* pool = nullptr) const;

  // Detect 'real' bit depth, which can be lower than nominal bit depth
  // (this is common in PNG), returns 'real' bit depth
  size_t DetectRealBitdepth() const;

  // -- ALPHA

  void SetAlpha(ImageU&& alpha, bool alpha_is_premultiplied);
  bool HasAlpha() const { return alpha_.xsize() != 0; }
  bool AlphaIsPremultiplied() const { return alpha_is_premultiplied_; }
  const ImageU& alpha() const {
    JXL_ASSERT(HasAlpha());
    return alpha_;
  }

  void PremultiplyAlphaIfNeeded(ThreadPool* pool = nullptr);

  // Reverts SetAlpha AND sets metadata->alpha_bits to 0. Called after noticing
  // that all alpha values are opaque.
  void RemoveAlpha();

  // -- DEPTH

  void SetDepth(ImageU&& depth);
  bool HasDepth() const {
    return metadata_->m2.Find(ExtraChannel::kDepth) != nullptr;
  }
  const ImageU& depth() const {
    JXL_ASSERT(HasDepth());
    const size_t ec = metadata_->m2.Find(ExtraChannel::kDepth) -
                      metadata_->m2.extra_channel_info.data();
    return extra_channels_[ec];
  }
  // Returns the dimensions of the depth image. Do not call if !HasDepth.
  size_t DepthSize(size_t size) const {
    return metadata_->m2.Find(ExtraChannel::kDepth)->Size(size);
  }

  // -- EXTRA CHANNELS

  // Extra channels of unknown interpretation (e.g. spot colors).
  void SetExtraChannels(std::vector<ImageU>&& extra_channels);
  bool HasExtraChannels() const { return !extra_channels_.empty(); }
  const std::vector<ImageU>& extra_channels() const {
    JXL_ASSERT(HasExtraChannels());
    return extra_channels_;
  }

  const ImageMetadata* metadata() const { return metadata_; }

  void VerifyMetadata() const;

  void SetDecodedBytes(size_t decoded_bytes) { decoded_bytes_ = decoded_bytes; }
  size_t decoded_bytes() const { return decoded_bytes_; }

  // -- JPEG transcoding:

  // Returns true if image does or will represent quantized DCT-8 coefficients,
  // stored in 8x8 pixel regions.
  bool IsJPEG() const { return is_jpeg; }

  bool is_jpeg = false;
  std::vector<int32_t> jpeg_quant_table;
  size_t jpeg_xsize = 0;  // image dimensions of input JPEG
  size_t jpeg_ysize = 0;  // (can be up to 7 smaller than color_ dimensions)
  // these fields are used to signal the input JPEG color space
  ColorTransform color_transform = ColorTransform::kNone;
  YCbCrChromaSubsampling chroma_subsampling = YCbCrChromaSubsampling::k444;

 private:
  // Called after any Set* to ensure their sizes are compatible.
  void VerifySizes() const;

  // Required for TransformTo so that an ImageBundle is self-sufficient. Always
  // points to the same thing, but cannot be const-pointer because that prevents
  // the compiler from generating a move ctor. Mostly pointer-to-const except as
  // required by RemoveAlpha.
  ImageMetadata* metadata_;

  // Initialized by Set*:
  Image3F color_;  // If empty, planes_ is not; all planes equal if IsGray().
  ColorEncoding c_current_;  // of color_

  // Initialized by SetAlpha.
  // TODO(janwas): move to extra channel
  ImageU alpha_;  // Empty or same size as color_.
  bool alpha_is_premultiplied_ = false;

  // Initialized by SetPlanes; size = ImageMetadata.num_extra_channels
  // TODO(janwas): change to pixel_type
  std::vector<ImageU> extra_channels_;

  // How many bytes of the input were actually read.
  size_t decoded_bytes_ = 0;
};

// Does color transformation from in.c_current() to c_desired if the color
// encodings are different, or nothing if they are already the same.
// If color transformation is done, stores the transformed values into store and
// sets the out pointer to store, else leaves store untouched and sets the out
// pointer to &in.
// Returns false if color transform fails.
Status TransformIfNeeded(const ImageBundle& in, const ColorEncoding& c_desired,
                         ThreadPool* pool, ImageBundle* store,
                         const ImageBundle** out);

}  // namespace jxl

#endif  // JXL_IMAGE_BUNDLE_H_
