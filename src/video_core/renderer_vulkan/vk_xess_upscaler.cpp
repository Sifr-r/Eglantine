// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_xess_upscaler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

#if defined(__has_include)
#if __has_include(<xess/xess_vk.h>)
#include <xess/xess.h>
#include <xess/xess_vk.h>
#define EGLANTINE_HAS_XESS_SDK 1
#elif __has_include(<xess_vk.h>)
#include <xess.h>
#include <xess_vk.h>
#define EGLANTINE_HAS_XESS_SDK 1
#endif
#endif

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;

#ifndef EGLANTINE_HAS_XESS_SDK
using xess_context_handle_t = void*;
using xess_result_t = s32;
using xess_quality_settings_t = u32;

constexpr xess_result_t XESS_RESULT_SUCCESS = 0;
constexpr xess_quality_settings_t XESS_QUALITY_SETTING_ULTRA_PERFORMANCE = 100;
constexpr xess_quality_settings_t XESS_QUALITY_SETTING_PERFORMANCE = 101;
constexpr xess_quality_settings_t XESS_QUALITY_SETTING_BALANCED = 102;
constexpr xess_quality_settings_t XESS_QUALITY_SETTING_QUALITY = 103;
constexpr xess_quality_settings_t XESS_QUALITY_SETTING_ULTRA_QUALITY = 104;
constexpr xess_quality_settings_t XESS_QUALITY_SETTING_AA = 106;

constexpr u32 XESS_INIT_FLAG_NONE = 0;
constexpr u32 XESS_INIT_FLAG_INVERTED_DEPTH = 1U << 2;
constexpr u32 XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE = 1U << 3;
constexpr u32 XESS_INIT_FLAG_LDR_INPUT_COLOR = 1U << 4;

struct xess_version_t {
    u16 major;
    u16 minor;
    u16 patch;
    u16 reserved;
};

struct xess_2d_t {
    u32 x;
    u32 y;
};

struct xess_vk_image_view_info {
    VkImageView imageView;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
    VkFormat format;
    u32 width;
    u32 height;
};

struct xess_vk_init_params_t {
    xess_2d_t outputResolution;
    xess_quality_settings_t qualitySetting;
    u32 initFlags;
    VkDeviceMemory tempBufferHeap;
    u32 bufferHeapOffset;
    VkDeviceMemory tempTextureHeap;
    u32 textureHeapOffset;
    VkPipelineCache pipelineCache;
};

struct xess_vk_execute_params_t {
    xess_vk_image_view_info colorTexture;
    xess_vk_image_view_info velocityTexture;
    xess_vk_image_view_info depthTexture;
    xess_vk_image_view_info exposureScaleTexture;
    xess_vk_image_view_info responsivePixelMaskTexture;
    xess_vk_image_view_info outputTexture;
    xess_2d_t inputColorBaseCoord;
    xess_2d_t inputMotionVectorBaseCoord;
    xess_2d_t inputDepthBaseCoord;
    xess_2d_t inputResponsiveMaskBaseCoord;
    xess_2d_t reserved0;
    xess_2d_t outputColorBaseCoord;
    f32 jitterOffsetX;
    f32 jitterOffsetY;
    f32 exposureScale;
    u32 resetHistory;
    u32 inputWidth;
    u32 inputHeight;
};
#endif

using PfnXessGetVersion = xess_result_t (*)(xess_version_t*);
using PfnXessVkCreateContext = xess_result_t (*)(VkInstance, VkPhysicalDevice, VkDevice,
                                                 xess_context_handle_t*);
using PfnXessDestroyContext = xess_result_t (*)(xess_context_handle_t);
using PfnXessVkInit = xess_result_t (*)(xess_context_handle_t, const xess_vk_init_params_t*);
using PfnXessVkBuildPipelines = xess_result_t (*)(xess_context_handle_t, VkPipelineCache, u8, u32);
using PfnXessVkExecute = xess_result_t (*)(xess_context_handle_t, VkCommandBuffer,
                                           const xess_vk_execute_params_t*);
using PfnXessSetJitterScale = xess_result_t (*)(xess_context_handle_t, f32, f32);
using PfnXessSetVelocityScale = xess_result_t (*)(xess_context_handle_t, f32, f32);

bool HasQualifiedSdkAbi() {
#ifdef EGLANTINE_HAS_XESS_SDK
    return true;
#else
    const char* const experimental = std::getenv("EDEN_NX2_XESS_EXPERIMENTAL_ABI");
    return experimental != nullptr && experimental[0] == '1';
#endif
}

std::unique_ptr<Common::DynamicLibrary> TryLoadXessLibrary() {
    auto lib = std::make_unique<Common::DynamicLibrary>();
#ifdef _WIN32
    constexpr std::array<const char*, 2> candidates{"libxess.dll", "xess.dll"};
#elif defined(__APPLE__)
    constexpr std::array<const char*, 2> candidates{"libxess.dylib", "libxess.2.dylib"};
#else
    constexpr std::array<const char*, 3> candidates{"libxess.so.2", "libxess.so", "xess.so"};
#endif
    if (const char* override_path = std::getenv("EDEN_NX2_XESS_PATH");
        override_path != nullptr && override_path[0] != '\0') {
        if (lib->Open(override_path)) {
            return lib;
        }
    }
    const std::filesystem::path xess_dir =
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "xess";
    for (const char* name : candidates) {
        std::error_code ec;
        const std::filesystem::path local_candidate = xess_dir / name;
        if (std::filesystem::exists(local_candidate, ec)) {
            const std::string utf8 = local_candidate.string();
            if (lib->Open(utf8.c_str())) {
                return lib;
            }
        }
        if (lib->Open(name)) {
            return lib;
        }
    }
    return nullptr;
}

xess_quality_settings_t ModeFor(VkExtent2D render, VkExtent2D upscale) {
    const f32 ratio = static_cast<f32>(render.width) / static_cast<f32>(upscale.width);
    if (ratio >= 0.99f) {
        return XESS_QUALITY_SETTING_AA;
    }
    if (ratio >= 0.76f) {
        return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    }
    if (ratio >= 0.65f) {
        return XESS_QUALITY_SETTING_QUALITY;
    }
    if (ratio >= 0.57f) {
        return XESS_QUALITY_SETTING_BALANCED;
    }
    if (ratio >= 0.49f) {
        return XESS_QUALITY_SETTING_PERFORMANCE;
    }
    return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
}

xess_quality_settings_t ConfiguredMode(VkExtent2D render, VkExtent2D upscale) {
    switch (Settings::values.nvidia_dlss_quality.GetValue()) {
    case Settings::DlssQuality::Performance:
        return XESS_QUALITY_SETTING_PERFORMANCE;
    case Settings::DlssQuality::Balanced:
        return XESS_QUALITY_SETTING_BALANCED;
    case Settings::DlssQuality::Quality:
        return XESS_QUALITY_SETTING_QUALITY;
    case Settings::DlssQuality::DLAA:
        return XESS_QUALITY_SETTING_AA;
    default:
        return ModeFor(render, upscale);
    }
}

VkImageMemoryBarrier Barrier(const XeSSUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

xess_vk_image_view_info MakeImageViewInfo(const XeSSUpscaler::Image& img, VkExtent2D extent) {
    return xess_vk_image_view_info{
        .imageView = img.view,
        .image = img.image,
        .subresourceRange = img.range,
        .format = img.format,
        .width = extent.width,
        .height = extent.height,
    };
}

} // Anonymous namespace

struct XeSSUpscaler::RuntimeApi {
    PfnXessGetVersion get_version{};
    PfnXessVkCreateContext create_context{};
    PfnXessDestroyContext destroy_context{};
    PfnXessVkInit init{};
    PfnXessVkBuildPipelines build_pipelines{};
    PfnXessVkExecute execute{};
    PfnXessSetJitterScale set_jitter_scale{};
    PfnXessSetVelocityScale set_velocity_scale{};
};

bool XeSSUpscaler::IsSupported(const Device& device) {
    if (!device.IsFormatSupported(OUTPUT_FORMAT,
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
                                  FormatType::Optimal)) {
        return false;
    }
    const auto lib = TryLoadXessLibrary();
    if (!lib || !lib->IsOpen()) {
        return false;
    }
    PfnXessVkCreateContext create_fn{};
    PfnXessVkInit init_fn{};
    PfnXessVkExecute execute_fn{};
    PfnXessDestroyContext destroy_fn{};
    if (!lib->GetSymbol("xessVKCreateContext", &create_fn) ||
        !lib->GetSymbol("xessVKInit", &init_fn) ||
        !lib->GetSymbol("xessVKExecute", &execute_fn) ||
        !lib->GetSymbol("xessDestroyContext", &destroy_fn)) {
        return false;
    }
    xess_version_t version{};
    PfnXessGetVersion version_fn{};
    if (lib->GetSymbol("xessGetVersion", &version_fn) && version_fn(&version) == XESS_RESULT_SUCCESS) {
        if (version.major < 2) {
            LOG_INFO(Render_Vulkan, "Intel XeSS v{}.{}.{} is older than v2.0 Vulkan runtime",
                     version.major, version.minor, version.patch);
            return false;
        }
        if (!HasQualifiedSdkAbi()) {
            LOG_INFO(Render_Vulkan,
                     "Intel XeSS v{}.{}.{} runtime detected; set EDEN_NX2_XESS_EXPERIMENTAL_ABI=1 "
                     "or build with Intel XeSS SDK headers to activate it",
                     version.major, version.minor, version.patch);
            return false;
        }
        return true;
    }
    return HasQualifiedSdkAbi();
}

XeSSUpscaler::XeSSUpscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      library{TryLoadXessLibrary()}, api{std::make_unique<RuntimeApi>()} {
    if (!library || !library->IsOpen() || !HasQualifiedSdkAbi()) {
        throw vk::Exception(VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    const bool loaded = library->GetSymbol("xessGetVersion", &api->get_version) &&
                        library->GetSymbol("xessVKCreateContext", &api->create_context) &&
                        library->GetSymbol("xessDestroyContext", &api->destroy_context) &&
                        library->GetSymbol("xessVKInit", &api->init) &&
                        library->GetSymbol("xessVKExecute", &api->execute);
    if (!loaded) {
        throw vk::Exception(VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    void(library->GetSymbol("xessVKBuildPipelines", &api->build_pipelines));
    void(library->GetSymbol("xessSetJitterScale", &api->set_jitter_scale));
    void(library->GetSymbol("xessSetVelocityScale", &api->set_velocity_scale));

    xess_context_handle_t raw_ctx{};
    const xess_result_t create_res =
        api->create_context(device.GetInstance(), device.GetPhysical(), *device.GetLogical(),
                            &raw_ctx);
    if (create_res < XESS_RESULT_SUCCESS || raw_ctx == nullptr) {
        LOG_WARNING(Render_Vulkan, "Intel XeSS context creation failed (result {})", create_res);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    context = raw_ctx;

    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 0.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

XeSSUpscaler::~XeSSUpscaler() {
    if (context && api && api->destroy_context) {
        scheduler.Finish();
        api->destroy_context(static_cast<xess_context_handle_t>(context));
        context = nullptr;
    }
}

void XeSSUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish();
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange{
                                       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1,
                                   },
                               });
    });
}

bool XeSSUpscaler::EnsureInitialized(const Inputs& inputs) {
    const xess_quality_settings_t quality =
        ConfiguredMode(inputs.render_size, inputs.upscale_size);
    const bool needs_reinit = !initialized ||
                              inputs.upscale_size.width != upscale_extent.width ||
                              inputs.upscale_size.height != upscale_extent.height ||
                              quality != current_quality ||
                              inputs.depth_inverted != depth_inverted || inputs.hdr != hdr;
    if (!output_image || inputs.upscale_size.width != upscale_extent.width ||
        inputs.upscale_size.height != upscale_extent.height) {
        CreateOutput(inputs.upscale_size);
    }
    render_extent = inputs.render_size;
    upscale_extent = inputs.upscale_size;
    if (!needs_reinit) {
        return true;
    }

    u32 init_flags = XESS_INIT_FLAG_NONE;
    if (inputs.depth_inverted) {
        init_flags |= XESS_INIT_FLAG_INVERTED_DEPTH;
    }
    if (inputs.hdr) {
        init_flags |= XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
    } else {
        init_flags |= XESS_INIT_FLAG_LDR_INPUT_COLOR;
    }

    if (api->build_pipelines) {
        void(api->build_pipelines(static_cast<xess_context_handle_t>(context),
                                  device.StaticPipelineCache(), 0, init_flags));
    }

    const xess_vk_init_params_t init_params{
        .outputResolution = {inputs.upscale_size.width, inputs.upscale_size.height},
        .qualitySetting = quality,
        .initFlags = init_flags,
        .tempBufferHeap = VK_NULL_HANDLE,
        .bufferHeapOffset = 0,
        .tempTextureHeap = VK_NULL_HANDLE,
        .textureHeapOffset = 0,
        .pipelineCache = device.StaticPipelineCache(),
    };
    const xess_result_t init_res =
        api->init(static_cast<xess_context_handle_t>(context), &init_params);
    if (init_res < XESS_RESULT_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "Intel XeSS init failed (result {}); falling back to FSR 3.1",
                  init_res);
        failed.store(true);
        return false;
    }
    if (api->set_jitter_scale) {
        void(api->set_jitter_scale(static_cast<xess_context_handle_t>(context), 1.0f, 1.0f));
    }
    current_quality = quality;
    depth_inverted = inputs.depth_inverted;
    hdr = inputs.hdr;
    initialized = true;
    LOG_INFO(Render_Vulkan, "Intel XeSS: {}x{} -> {}x{}, quality {}", inputs.render_size.width,
             inputs.render_size.height, inputs.upscale_size.width, inputs.upscale_size.height,
             quality);
    return true;
}

bool XeSSUpscaler::Upscale(const Inputs& in) {
    if (failed.load() || in.render_size.width == 0 || in.render_size.height == 0 ||
        in.upscale_size.width == 0 || in.upscale_size.height == 0) {
        return false;
    }
    const bool first = !initialized;
    if (!EnsureInitialized(in)) {
        return false;
    }
    const bool reset = in.reset || first;

    if (api->set_velocity_scale) {
        void(api->set_velocity_scale(static_cast<xess_context_handle_t>(context),
                                     in.motion_vector_scale[0], in.motion_vector_scale[1]));
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, reset, output_image_handle = *output_image,
                      output_view_handle = *output_view](vk::CommandBuffer cmdbuf) {
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        const Image output_desc{
            .view = output_view_handle,
            .image = output_image_handle,
            .format = OUTPUT_FORMAT,
            .range{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .barrier_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        };
        const xess_vk_execute_params_t exec_params{
            .colorTexture = MakeImageViewInfo(in.color, in.render_size),
            .velocityTexture = MakeImageViewInfo(in.motion_vectors, in.render_size),
            .depthTexture = MakeImageViewInfo(in.depth, in.render_size),
            .exposureScaleTexture = {},
            .responsivePixelMaskTexture = {},
            .outputTexture = MakeImageViewInfo(output_desc, in.upscale_size),
            .inputColorBaseCoord = {0, 0},
            .inputMotionVectorBaseCoord = {0, 0},
            .inputDepthBaseCoord = {0, 0},
            .inputResponsiveMaskBaseCoord = {0, 0},
            .reserved0 = {0, 0},
            .outputColorBaseCoord = {0, 0},
            .jitterOffsetX = in.jitter[0],
            .jitterOffsetY = in.jitter[1],
            .exposureScale = 1.0f,
            .resetHistory = reset ? 1U : 0U,
            .inputWidth = in.render_size.width,
            .inputHeight = in.render_size.height,
        };
        const xess_result_t result = api->execute(
            static_cast<xess_context_handle_t>(context), *cmdbuf, &exec_params);
        if (result < XESS_RESULT_SUCCESS && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan,
                      "Intel XeSS execution failed (result {}); falling back to FSR 3.1", result);
        }

        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        const VkMemoryBarrier output_written{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, output_written, {},
                               to_general);
        if (result < XESS_RESULT_SUCCESS) {
            cmdbuf.ClearColorImage(output_image_handle, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{}, output_desc.range);
        }
    });
    scheduler.InvalidateState();
    return true;
}

} // namespace Vulkan
