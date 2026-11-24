// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_cooperative_hmma_test.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using Shader::Backend::SPIRV::COOPERATIVE_HMMA_TEST_LANE_WORDS;

constexpr u32 LANES = GuestWarpSize;
constexpr u32 BUFFER_WORDS = LANES * COOPERATIVE_HMMA_TEST_LANE_WORDS;
constexpr u32 CALIBRATION_MAGIC = 0x484D4D41U; // "HMMA"
constexpr u32 CALIBRATION_VERSION = 1U;

struct CachedCalibrationRecord {
    u32 magic{CALIBRATION_MAGIC};
    u32 version{CALIBRATION_VERSION};
    u32 device_id{};
    u32 driver_id{};
    u32 driver_version{};
    u32 spirv_version{};
    u32 tile_m{16};
    u32 tile_n{8};
    u32 tile_k{16};
    u32 subgroup_size{LANES};
    std::array<u32, 4> a_word_map{0, 2, 1, 3};
    u32 supported{};
};

bool IsValidWordMap(const std::array<u32, 4>& map) {
    std::array<bool, 4> seen{};
    for (const u32 idx : map) {
        if (idx >= 4 || seen[idx]) {
            return false;
        }
        seen[idx] = true;
    }
    return true;
}

std::filesystem::path CalibrationCachePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) / "coop_hmma_calibration.bin";
}

std::optional<CoopHmmaProfile> TryLoadCachedProfile(const Device& device, u32 tile_m, u32 tile_n,
                                                    u32 tile_k, u32 subgroup_size) {
    if (const char* recalibrate = std::getenv("EDEN_NX2_RECALIBRATE_COOP");
        recalibrate != nullptr && recalibrate[0] == '1') {
        return std::nullopt;
    }
    try {
        const std::filesystem::path path = CalibrationCachePath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return std::nullopt;
        }
        std::ifstream file{path, std::ios::binary};
        CachedCalibrationRecord rec{};
        if (!file.read(reinterpret_cast<char*>(&rec), sizeof(rec))) {
            return std::nullopt;
        }
        if (rec.magic != CALIBRATION_MAGIC || rec.version != CALIBRATION_VERSION ||
            rec.device_id != device.GetDeviceID() ||
            rec.driver_id != static_cast<u32>(device.GetDriverID()) ||
            rec.driver_version != device.GetDriverVersion() ||
            rec.spirv_version != device.SupportedSpirvVersion() || rec.tile_m != tile_m ||
            rec.tile_n != tile_n || rec.tile_k != tile_k ||
            rec.subgroup_size != subgroup_size || !IsValidWordMap(rec.a_word_map)) {
            return std::nullopt;
        }
        return CoopHmmaProfile{
            .supported = rec.supported != 0,
            .tile_m = rec.tile_m,
            .tile_n = rec.tile_n,
            .tile_k = rec.tile_k,
            .subgroup_size = rec.subgroup_size,
            .a_word_map = rec.a_word_map,
            .from_cache = true,
        };
    } catch (...) {
        return std::nullopt;
    }
}

void SaveCachedProfile(const Device& device, const CoopHmmaProfile& profile) {
    try {
        const std::filesystem::path path = CalibrationCachePath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return;
        }
        const CachedCalibrationRecord rec{
            .magic = CALIBRATION_MAGIC,
            .version = CALIBRATION_VERSION,
            .device_id = device.GetDeviceID(),
            .driver_id = static_cast<u32>(device.GetDriverID()),
            .driver_version = device.GetDriverVersion(),
            .spirv_version = device.SupportedSpirvVersion(),
            .tile_m = profile.tile_m,
            .tile_n = profile.tile_n,
            .tile_k = profile.tile_k,
            .subgroup_size = profile.subgroup_size,
            .a_word_map = profile.a_word_map,
            .supported = profile.supported ? 1U : 0U,
        };
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    } catch (...) {
        // Non-fatal: calibration still applies for the current session.
    }
}

/// The half-precision encoding of a small integer. Products and sums of the test's integers stay
/// exact in half precision, so the results compare bit for bit, without any rounding involved.
u16 IntegerToHalf(s32 value) {
    if (value == 0) {
        return 0;
    }
    const u32 magnitude = static_cast<u32>(value < 0 ? -value : value);
    const u32 exponent = static_cast<u32>(std::bit_width(magnitude)) - 1;
    const u32 mantissa = (magnitude << (10 - exponent)) & 0x3ff;
    return static_cast<u16>((value < 0 ? 0x8000U : 0U) | ((exponent + 15) << 10) | mantissa);
}

u32 PackHalves(s32 low, s32 high) {
    return IntegerToHalf(low) | (static_cast<u32>(IntegerToHalf(high)) << 16);
}

struct Matrices {
    std::array<std::array<s32, 16>, 16> a{};
    std::array<std::array<s32, 8>, 16> b{};
    std::array<std::array<s32, 8>, 16> c{};
    std::array<std::array<s32, 8>, 16> d{};
};

Matrices MakeMatrices(u32 seed = 0x2545f491U) {
    // Every element differs from most others, so that any misplaced element changes the results.
    u32 state = seed;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<s32>((state >> 16) % 7) - 3;
    };
    Matrices m;
    for (auto& row : m.a) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (auto& row : m.b) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (auto& row : m.c) {
        for (s32& value : row) {
            value = next();
        }
    }
    for (size_t row = 0; row < 16; ++row) {
        for (size_t column = 0; column < 8; ++column) {
            s32 sum = m.c[row][column];
            for (size_t k = 0; k < 16; ++k) {
                sum += m.a[row][k] * m.b[k][column];
            }
            m.d[row][column] = sum;
        }
    }
    return m;
}

/// Writes one lane's guest registers, in the layout of mma.m16n8k16: with g = lane / 4 and
/// t = lane % 4, a lane holds rows g and g + 8 of A, C and D, and column g of B.
void WriteLane(const Matrices& m, u32 lane, std::span<u32> words) {
    const size_t g = lane >> 2;
    const size_t t = lane & 3;
    const size_t k = t * 2;
    words[0] = PackHalves(m.a[g][k], m.a[g][k + 1]);
    words[1] = PackHalves(m.a[g + 8][k], m.a[g + 8][k + 1]);
    words[2] = PackHalves(m.a[g][k + 8], m.a[g][k + 9]);
    words[3] = PackHalves(m.a[g + 8][k + 8], m.a[g + 8][k + 9]);
    words[4] = PackHalves(m.b[k][g], m.b[k + 1][g]);
    words[5] = PackHalves(m.b[k + 8][g], m.b[k + 9][g]);
    words[6] = PackHalves(m.c[g][k], m.c[g][k + 1]);
    words[7] = PackHalves(m.c[g + 8][k], m.c[g + 8][k + 1]);
    words[8] = 0;
    words[9] = 0;
}

bool SameHalf(u32 got, s32 expected) {
    const u32 want = IntegerToHalf(expected);
    // A sum that cancels to zero may have either sign.
    return got == want || (expected == 0 && (got & 0x7fff) == 0);
}

u32 CountWrongLanes(const Matrices& m, std::span<const u32> words) {
    u32 wrong = 0;
    for (u32 lane = 0; lane < LANES; ++lane) {
        const size_t g = lane >> 2;
        const size_t k = (lane & 3) * 2;
        const auto lane_words = words.subspan(lane * COOPERATIVE_HMMA_TEST_LANE_WORDS);
        const u32 d0 = lane_words[8];
        const u32 d1 = lane_words[9];
        if (!SameHalf(d0 & 0xffff, m.d[g][k]) || !SameHalf(d0 >> 16, m.d[g][k + 1]) ||
            !SameHalf(d1 & 0xffff, m.d[g + 8][k]) || !SameHalf(d1 >> 16, m.d[g + 8][k + 1])) {
            ++wrong;
        }
    }
    return wrong;
}

std::vector<std::array<u32, 4>> BuildCandidateWordMaps() {
    std::vector<std::array<u32, 4>> candidates;
    candidates.reserve(24);
    constexpr std::array<u32, 4> nvidia_default{0, 2, 1, 3};
    constexpr std::array<u32, 4> linear_identity{0, 1, 2, 3};
    candidates.push_back(nvidia_default);
    candidates.push_back(linear_identity);

    std::array<u32, 4> perm{0, 1, 2, 3};
    while (std::next_permutation(perm.begin(), perm.end())) {
        if (perm != nvidia_default && perm != linear_identity) {
            candidates.push_back(perm);
        }
    }
    return candidates;
}

CoopHmmaProfile RunCalibration(const Device& device, u32 tile_m, u32 tile_n, u32 tile_k,
                               u32 subgroup_size) {
    const auto& logical = device.GetLogical();
    const VkDescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    };
    const vk::DescriptorSetLayout set_layout =
        logical.CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = 1,
            .pBindings = &binding,
        });
    const VkDescriptorSetLayout set_layout_handle = *set_layout;
    const vk::PipelineLayout layout = logical.CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout_handle,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    });
    const VkDescriptorPoolSize pool_size{
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };
    const vk::DescriptorPool pool = logical.CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const vk::DescriptorSets sets = pool.Allocate(VkDescriptorSetAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = *pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout_handle,
    });
    const VkDescriptorSet set = sets[0];

    const MemoryAllocator allocator{device};
    vk::Buffer buffer = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = BUFFER_WORDS * sizeof(u32),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::Download);

    const VkDescriptorBufferInfo buffer_info{
        .buffer = *buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pImageInfo = nullptr,
        .pBufferInfo = &buffer_info,
        .pTexelBufferView = nullptr,
    };
    logical.UpdateDescriptorSets(write, {});

    // Its own submission: this runs while the renderer is being built, before the scheduler can
    // flush (it needs the query cache) and before anything else submits to the queue.
    const vk::CommandPool command_pool = logical.CreateCommandPool(VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    const vk::CommandBuffers command_buffers = command_pool.Allocate(1);
    const vk::CommandBuffer cmdbuf{command_buffers[0], device.GetDispatchLoader()};
    const vk::Fence fence = logical.CreateFence(VkFenceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    });

    const auto run_once = [&](const vk::Pipeline& pipeline, const Matrices& matrices) -> u32 {
        std::array<u32, BUFFER_WORDS> words{};
        for (u32 lane = 0; lane < LANES; ++lane) {
            WriteLane(matrices, lane,
                      std::span{words}.subspan(lane * COOPERATIVE_HMMA_TEST_LANE_WORDS,
                                               COOPERATIVE_HMMA_TEST_LANE_WORDS));
        }
        std::memcpy(buffer.Mapped().data(), words.data(), sizeof(words));
        buffer.Flush();

        cmdbuf.Begin(VkCommandBufferBeginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        });
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.Dispatch(1, 1, 1);
        const VkMemoryBarrier read_results{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                               read_results);
        cmdbuf.End();

        const VkCommandBuffer submitted = *cmdbuf;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &submitted,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        vk::Check(device.GetGraphicsQueue().Submit(submit, *fence));
        vk::Check(fence.Wait());
        fence.Reset();

        buffer.Invalidate();
        std::memcpy(words.data(), buffer.Mapped().data(), sizeof(words));
        return CountWrongLanes(matrices, words);
    };

    const Matrices primary_matrices = MakeMatrices(0x2545f491U);
    const Matrices verify_matrices = MakeMatrices(0x9E3779B9U);
    u32 best_wrong = LANES;

    for (const auto& candidate : BuildCandidateWordMaps()) {
        const std::vector<u32> code = Shader::Backend::SPIRV::EmitCooperativeHmmaTest(
            device.SupportedSpirvVersion(), candidate, tile_m, tile_n, tile_k);
        const vk::ShaderModule module = logical.CreateShaderModule(VkShaderModuleCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = code.size() * sizeof(u32),
            .pCode = code.data(),
        });
        const vk::Pipeline pipeline = logical.CreateComputePipeline(VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            .layout = *layout,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        });

        const u32 wrong = run_once(pipeline, primary_matrices);
        best_wrong = (std::min)(best_wrong, wrong);
        if (wrong == 0 && run_once(pipeline, verify_matrices) == 0) {
            LOG_INFO(Render_Vulkan,
                     "Guest tensor operations calibrated on cooperative matrices "
                     "(tile={}x{}x{}, a_word_map=[{}, {}, {}, {}])",
                     tile_m, tile_n, tile_k, candidate[0], candidate[1], candidate[2],
                     candidate[3]);
            return CoopHmmaProfile{
                .supported = true,
                .tile_m = tile_m,
                .tile_n = tile_n,
                .tile_k = tile_k,
                .subgroup_size = subgroup_size,
                .a_word_map = candidate,
                .from_cache = false,
            };
        }
    }

    LOG_WARNING(Render_Vulkan,
                "Cooperative matrices lay out elements incompatibly on this driver (best {} of "
                "{} lanes wrong): guest tensor operations stay in shader emulation",
                best_wrong, LANES);
    return CoopHmmaProfile{
        .supported = false,
        .tile_m = tile_m,
        .tile_n = tile_n,
        .tile_k = tile_k,
        .subgroup_size = subgroup_size,
        .a_word_map = {0, 2, 1, 3},
        .from_cache = false,
    };
}

} // Anonymous namespace

CoopHmmaProfile ProbeCooperativeHmma(const Device& device) {
    const auto& caps = device.GetCoopMatrixCapabilities();
    const u32 tile_m = caps.preferred_m != 0 ? caps.preferred_m : 16U;
    const u32 tile_n = caps.preferred_n != 0 ? caps.preferred_n : 8U;
    const u32 tile_k = caps.preferred_k != 0 ? caps.preferred_k : 16U;
    const u32 subgroup_size = caps.subgroup_size != 0 ? caps.subgroup_size : LANES;

    if (!device.IsCooperativeHmmaSupported() || tile_m != 16 || tile_n != 8 || tile_k != 16) {
        return CoopHmmaProfile{
            .supported = false,
            .tile_m = tile_m,
            .tile_n = tile_n,
            .tile_k = tile_k,
            .subgroup_size = subgroup_size,
            .a_word_map = {0, 2, 1, 3},
            .from_cache = false,
        };
    }

    if (const auto cached = TryLoadCachedProfile(device, tile_m, tile_n, tile_k, subgroup_size)) {
        if (cached->supported) {
            LOG_INFO(Render_Vulkan,
                     "Using cached cooperative matrix calibration (tile={}x{}x{}, "
                     "a_word_map=[{}, {}, {}, {}])",
                     cached->tile_m, cached->tile_n, cached->tile_k, cached->a_word_map[0],
                     cached->a_word_map[1], cached->a_word_map[2], cached->a_word_map[3]);
        }
        return *cached;
    }

    try {
        CoopHmmaProfile profile = RunCalibration(device, tile_m, tile_n, tile_k, subgroup_size);
        SaveCachedProfile(device, profile);
        return profile;
    } catch (const std::exception& exception) {
        LOG_WARNING(Render_Vulkan,
                    "Cooperative matrix self-test failed ({}): guest tensor operations stay in "
                    "shader emulation",
                    exception.what());
        return CoopHmmaProfile{
            .supported = false,
            .tile_m = tile_m,
            .tile_n = tile_n,
            .tile_k = tile_k,
            .subgroup_size = subgroup_size,
            .a_word_map = {0, 2, 1, 3},
            .from_cache = false,
        };
    }
}

bool TestCooperativeHmma(const Device& device) {
    return ProbeCooperativeHmma(device).supported;
}

} // namespace Vulkan
