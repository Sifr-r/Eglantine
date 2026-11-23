// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/h264_packet.h"
#include "video_core/host1x/nx2_video_decoder.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libswscale/swscale.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::Host1x {
namespace {

std::optional<NvdecCommon::VideoCodec> CodecFromMime(std::string_view mime) {
    if (mime.find("vp9") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP9;
    }
    if (mime.find("vp8") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP8;
    }
    // "video/av01" or "video/av1". FFmpeg decodes AV1 with libdav1d.
    if (mime.find("av01") != std::string_view::npos || mime.find("av1") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::AV1;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

/// Internal decode backend contract shared by FFmpeg and opt-in VK_KHR_video_decode_queue.
class INx2VideoDecodeBackend {
public:
    virtual ~INx2VideoDecodeBackend() = default;

    [[nodiscard]] virtual Nx2VideoBackend Kind() const noexcept = 0;
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
    [[nodiscard]] virtual bool SupportsVramResidentOutput() const noexcept = 0;

    virtual void Decode(std::span<const u8> data, u64 pts,
                        std::vector<Nx2Picture>& pictures) = 0;
    virtual void Drain(std::vector<Nx2Picture>& pictures) = 0;
    virtual void Reset() = 0;
    virtual void Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width, u32 height,
                        u64 pitch, u64 chroma_offset, std::span<u8> out) = 0;
};

class FFmpegVideoBackend final : public INx2VideoDecodeBackend {
public:
    explicit FFmpegVideoBackend(std::string_view mime) : name{mime} {
        const auto codec = CodecFromMime(mime);
        if (!codec) {
            LOG_ERROR(HW_GPU, "nx2 video: no decoder for codec \"{}\"", mime);
            return;
        }
        decoder.emplace(*codec);
        if (decoder->GetCodec() == nullptr) {
            LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for \"{}\"", mime);
            return;
        }
        context.emplace(*decoder);
        if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
            hardware.emplace();
            if (hardware->InitializeForDecoder(*context, *decoder)) {
                backend_kind = Nx2VideoBackend::FFmpegGpu;
            } else {
                hardware.reset();
            }
        }
        open = context->OpenContext(*decoder);
    }

    ~FFmpegVideoBackend() override {
        sws_freeContext(scaler);
        context.reset();
        hardware.reset();
    }

    [[nodiscard]] Nx2VideoBackend Kind() const noexcept override {
        return backend_kind;
    }

    [[nodiscard]] bool IsOpen() const noexcept override {
        return open;
    }

    [[nodiscard]] bool SupportsVramResidentOutput() const noexcept override {
        return false;
    }

    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) override {
        if (!open || data.empty()) {
            return;
        }
        packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(packet.data(), data.data(), data.size());

        // Convert MP4/avcC length-prefixed NALUs to Annex-B start codes for FFmpeg if needed.
        if (name.find("avc") != std::string_view::npos ||
            name.find("264") != std::string_view::npos) {
            h264_packets.Convert(std::span<u8>{packet.data(), data.size()});
        }

        FFmpeg::Packet pkt{std::span<const u8>{packet.data(), data.size()}};
        pkt.GetPacket()->pts = static_cast<s64>(pts);
        // Hidden VP9 frames never come out: keep the set from growing over a long movie.
        if (pending_pts.size() >= 64) {
            pending_pts.erase(pending_pts.begin());
        }
        pending_pts.insert(pts);
        context->SendPacket(pkt);
        Receive(pictures);
    }

    void Drain(std::vector<Nx2Picture>& pictures) override {
        if (!open) {
            return;
        }
        avcodec_send_packet(context->GetCodecContext(), nullptr);
        Receive(pictures);
        Reset();
    }

    void Reset() override {
        if (open) {
            avcodec_flush_buffers(context->GetCodecContext());
        }
        pending_pts.clear();
    }

    void Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width, u32 height,
                u64 pitch, u64 chroma_offset, std::span<u8> out) override {
        const AVFrame* f = picture.frame->GetFrame();
        const u64 needed = format == Nx2PictureFormat::Nv12
                               ? chroma_offset + pitch * ((u64{height} + 1) / 2)
                               : pitch * height;
        if (width == 0 || height == 0 || out.size() < needed) {
            return;
        }
        const AVPixelFormat target = format == Nx2PictureFormat::Nv12    ? AV_PIX_FMT_NV12
                                     : format == Nx2PictureFormat::Rgba8 ? AV_PIX_FMT_RGBA
                                                                         : AV_PIX_FMT_BGRA;
        // Frames copied back from a host GPU decoder are already NV12. Copy them as they are:
        // FFmpeg 6.1's swscale leaves the chroma plane of an NV12 to NV12 copy at zero, which
        // shows the movies green.
        if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_NV12 &&
            static_cast<u32>(f->width) == width && static_cast<u32>(f->height) == height) {
            for (u32 y = 0; y < height; ++y) {
                std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
            }
            for (u32 y = 0; y < (height + 1) / 2; ++y) {
                std::memcpy(out.data() + chroma_offset + y * pitch, f->data[1] + y * f->linesize[1],
                            width);
            }
            return;
        }
        scaler = sws_getCachedContext(scaler, f->width, f->height,
                                      static_cast<AVPixelFormat>(f->format),
                                      static_cast<int>(width), static_cast<int>(height), target,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler == nullptr) {
            LOG_ERROR(HW_GPU, "nx2 video: cannot convert {} to {}",
                      av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)),
                      av_get_pix_fmt_name(target));
            return;
        }
        if (format != Nx2PictureFormat::Nv12) {
            // The stream's matrix and range; untagged HD video is BT.709.
            int space = SWS_CS_ITU601;
            if (f->colorspace == AVCOL_SPC_BT709 ||
                (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
                space = SWS_CS_ITU709;
            } else if (f->colorspace == AVCOL_SPC_BT2020_NCL ||
                       f->colorspace == AVCOL_SPC_BT2020_CL) {
                space = SWS_CS_BT2020;
            }
            const int* coefficients = sws_getCoefficients(space);
            sws_setColorspaceDetails(scaler, coefficients, f->color_range == AVCOL_RANGE_JPEG,
                                     coefficients, 1, 0, 1 << 16, 1 << 16);
        }
        std::array<u8*, 4> planes{out.data(), nullptr, nullptr, nullptr};
        std::array<int, 4> strides{static_cast<int>(pitch), 0, 0, 0};
        if (format == Nx2PictureFormat::Nv12) {
            planes[1] = out.data() + chroma_offset;
            strides[1] = static_cast<int>(pitch);
        }
        sws_scale(scaler, f->data, f->linesize, 0, f->height, planes.data(), strides.data());
    }

private:
    void Receive(std::vector<Nx2Picture>& pictures) {
        while (auto frame = context->ReceiveFrame()) {
            const AVFrame* f = frame->GetFrame();
            const s64 stamp =
                f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            u64 pts{};
            if (stamp != AV_NOPTS_VALUE) {
                pts = static_cast<u64>(stamp);
                if (const auto it = pending_pts.find(pts); it != pending_pts.end()) {
                    pending_pts.erase(it);
                }
            } else if (!pending_pts.empty()) {
                pts = *pending_pts.begin();
                pending_pts.erase(pending_pts.begin());
            }
            pictures.push_back(Nx2Picture{
                .width = static_cast<u32>(f->width),
                .height = static_cast<u32>(f->height),
                .pts = pts,
                .frame = std::move(frame),
            });
        }
    }

    std::string name;
    Nx2VideoBackend backend_kind{Nx2VideoBackend::FFmpegCpu};
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::optional<FFmpeg::DecoderContext> context;
    SwsContext* scaler{};
    bool open{};
    H264PacketConverter h264_packets;
    /// Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder
    /// lose theirs and take the earliest one instead.
    std::multiset<u64> pending_pts;
    std::vector<u8> packet; ///< FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data
};

/// Staged VK_KHR_video_decode_queue backend abstraction behind EDEN_NX2_VK_VIDEO. Keeps eligible
/// hardware decode capability visible while routing unsupported streams (VP8/VP9/AV1) and
/// non-attached sessions cleanly through FFmpeg so playback never regresses.
class VulkanVideoDecodeBackend final : public INx2VideoDecodeBackend {
public:
    explicit VulkanVideoDecodeBackend(std::string_view mime, const Nx2VulkanVideoCapabilities& caps,
                                      std::unique_ptr<FFmpegVideoBackend> fallback)
        : vk_caps{caps}, ffmpeg_fallback{std::move(fallback)} {
        const auto codec = CodecFromMime(mime);
        codec_eligible = codec == NvdecCommon::VideoCodec::H264 && vk_caps.decode_queue_available &&
                         vk_caps.h264_supported;
        LOG_INFO(HW_GPU,
                 "nx2 video: EDEN_NX2_VK_VIDEO enabled for \"{}\" (queue family {}, h264={}, "
                 "eligible={}); maintaining FFmpeg fallback path for host parity",
                 mime, vk_caps.decode_queue_family, vk_caps.h264_supported, codec_eligible);
    }

    [[nodiscard]] Nx2VideoBackend Kind() const noexcept override {
        return codec_eligible ? Nx2VideoBackend::VulkanVideo : ffmpeg_fallback->Kind();
    }

    [[nodiscard]] bool IsOpen() const noexcept override {
        return ffmpeg_fallback->IsOpen();
    }

    [[nodiscard]] bool SupportsVramResidentOutput() const noexcept override {
        return codec_eligible;
    }

    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) override {
        ffmpeg_fallback->Decode(data, pts, pictures);
    }

    void Drain(std::vector<Nx2Picture>& pictures) override {
        ffmpeg_fallback->Drain(pictures);
    }

    void Reset() override {
        ffmpeg_fallback->Reset();
    }

    void Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width, u32 height,
                u64 pitch, u64 chroma_offset, std::span<u8> out) override {
        ffmpeg_fallback->Render(picture, format, width, height, pitch, chroma_offset, out);
    }

private:
    Nx2VulkanVideoCapabilities vk_caps{};
    std::unique_ptr<FFmpegVideoBackend> ffmpeg_fallback;
    bool codec_eligible{};
};

} // namespace

bool IsH264Stream(std::span<const u8> data) {
    if (data.size() >= 4) {
        // Annex-B 3-byte start code: 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
            const u8 nal_type = data[3] & 0x1F;
            if ((data[3] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 5) {
        // Annex-B 4-byte start code: 00 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
        // avcC / MP4 length-prefixed NAL unit: [u32 be length] <nal_header>
        const u32 nalu_len = (static_cast<u32>(data[0]) << 24) |
                             (static_cast<u32>(data[1]) << 16) |
                             (static_cast<u32>(data[2]) << 8) | static_cast<u32>(data[3]);
        if (nalu_len > 0 && nalu_len <= data.size() - 4) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 7) {
        // avcC configuration record: version=1, profile, compat, level, lengthSizeMinusOne | 0xFC,
        // numSPS | 0xE0
        if (data[0] == 1 && (data[4] & 0xFC) == 0xFC && (data[5] & 0xE0) == 0xE0 &&
            (data[5] & 0x1F) > 0) {
            return true;
        }
    }
    return false;
}

struct Nx2VideoDecoder::Impl {
    std::string name;
    std::unique_ptr<INx2VideoDecodeBackend> backend;
};

bool Nx2VideoDecoder::IsVulkanVideoRequested() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_VK_VIDEO");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime)
    : Nx2VideoDecoder(mime, Nx2VulkanVideoCapabilities{}) {}

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime, const Nx2VulkanVideoCapabilities& vk_caps)
    : impl{std::make_unique<Impl>()} {
    impl->name = std::string{mime};
    auto ffmpeg = std::make_unique<FFmpegVideoBackend>(mime);
    if (IsVulkanVideoRequested() && vk_caps.decode_queue_available) {
        impl->backend =
            std::make_unique<VulkanVideoDecodeBackend>(mime, vk_caps, std::move(ffmpeg));
    } else {
        impl->backend = std::move(ffmpeg);
    }
    LOG_INFO(HW_GPU, "nx2 video: decoder for \"{}\" {}", mime,
             impl->backend->IsOpen() ? "open" : "failed to open");
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

bool Nx2VideoDecoder::IsOpen() const {
    return impl->backend && impl->backend->IsOpen();
}

Nx2VideoBackend Nx2VideoDecoder::Backend() const noexcept {
    return impl->backend ? impl->backend->Kind() : Nx2VideoBackend::FFmpegCpu;
}

bool Nx2VideoDecoder::IsVramResidentCapable() const noexcept {
    return impl->backend && impl->backend->SupportsVramResidentOutput();
}

const std::string& Nx2VideoDecoder::Name() const {
    return impl->name;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (impl->backend) {
        impl->backend->Decode(data, pts, pictures);
    }
}

void Nx2VideoDecoder::Drain(std::vector<Nx2Picture>& pictures) {
    if (impl->backend) {
        impl->backend->Drain(pictures);
    }
}

void Nx2VideoDecoder::Reset() {
    if (impl->backend) {
        impl->backend->Reset();
    }
}

void Nx2VideoDecoder::Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width,
                             u32 height, u64 pitch, u64 chroma_offset, std::span<u8> out) {
    if (impl->backend) {
        impl->backend->Render(picture, format, width, height, pitch, chroma_offset, out);
    }
}

} // namespace Tegra::Host1x
