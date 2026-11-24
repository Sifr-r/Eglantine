// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/gpu_logging/gpu_logging.h"

#include <algorithm>
#include <fmt/format.h>
#include <mutex>
#include <thread>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/settings.h"

namespace GPU::Logging {

GPULogger& GPULogger::GetInstance() {
    static GPULogger instance;
    return instance;
}

GPULogger::GPULogger() = default;

GPULogger::~GPULogger() {
    ShutdownInternal(false);
}

void GPULogger::Initialize(LogLevel level, DriverType driver) {
    std::lock_guard lifecycle_lock(lifecycle_mutex);
    if (initialized.load(std::memory_order_acquire)) {
        LOG_WARNING(Render_Vulkan, "[GPU Logging] Already initialized");
        return;
    }

    current_level.store(level, std::memory_order_release);
    detected_driver.store(driver, std::memory_order_release);

    if (level == LogLevel::Off) {
        return;
    }

    // Create log directory
    using namespace Common::FS;
    const auto& log_dir = GetEdenPath(EdenPath::LogDir);
    [[maybe_unused]] const bool log_dir_created = CreateDir(log_dir);

    // Create GPU crashes directory
    const auto crashes_dir = log_dir / "gpu_crashes";
    [[maybe_unused]] const bool crashes_dir_created = CreateDir(crashes_dir);

    // Open GPU log file
    const auto gpu_log_path = log_dir / "eden_gpu.log";

    // Rotate old log
    const auto old_log_path = log_dir / "eden_gpu.log.old.txt";
    RemoveFile(old_log_path);
    [[maybe_unused]] const bool log_renamed = RenameFile(gpu_log_path, old_log_path);

    // Initialize ring buffer
    size_t configured_ring_size = 0;
    {
        std::lock_guard ring_lock(ring_buffer_mutex);
        ring_buffer_size = (std::max)(ring_buffer_size, size_t{1});
        configured_ring_size = ring_buffer_size;
        call_ring_buffer.assign(ring_buffer_size, VulkanCallEntry{});
        ring_buffer_index = 0;
    }

    // Write header
    const char* driver_name = "Unknown";
    switch (driver) {
    case DriverType::Turnip:
        driver_name = "Turnip (Mesa Freedreno)";
        break;
    case DriverType::Qualcomm:
        driver_name = "Qualcomm Proprietary";
        break;
    default:
        driver_name = "Unknown";
        break;
    }

    const char* level_name = "Unknown";
    switch (level) {
    case LogLevel::Off:
        level_name = "Off";
        break;
    case LogLevel::Errors:
        level_name = "Errors";
        break;
    case LogLevel::Standard:
        level_name = "Standard";
        break;
    case LogLevel::Verbose:
        level_name = "Verbose";
        break;
    case LogLevel::All:
        level_name = "All";
        break;
    }

    const auto header = fmt::format(
        "=== Eden GPU Logging Started ===\n"
        "Timestamp: {}\n"
        "Log Level: {}\n"
        "Driver: {}\n"
        "Ring Buffer Size: {}\n"
        "================================\n\n",
        FormatTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())),
        level_name, driver_name, configured_ring_size);

    {
        std::lock_guard file_lock(file_mutex);
        gpu_log_file = std::make_unique<Common::FS::IOFile>(
            gpu_log_path, Common::FS::FileAccessMode::Write, Common::FS::FileType::TextFile);

        if (!gpu_log_file->IsOpen()) {
            gpu_log_file.reset();
            LOG_ERROR(Render_Vulkan, "[GPU Logging] Failed to open GPU log file");
            return;
        }

        bytes_written.store(static_cast<u64>(gpu_log_file->WriteString(header)),
                            std::memory_order_relaxed);
    }

    // Note: Crash handler is initialized independently in EmulationSession::InitializeSystem()
    // to ensure it remains active even if Vulkan device initialization fails

    initialized.store(true, std::memory_order_release);
    LOG_INFO(Render_Vulkan, "[GPU Logging] Initialized with level: {}, driver: {}", level_name,
             driver_name);
}

void GPULogger::Shutdown() {
    ShutdownInternal(true);
}

void GPULogger::ShutdownInternal(bool log_message) {
    std::lock_guard lifecycle_lock(lifecycle_mutex);
    if (!initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // Write statistics
    const auto stats = fmt::format(
        "\n=== GPU Logging Statistics ===\n"
        "Total Vulkan Calls: {}\n"
        "Total Memory Allocations: {}\n"
        "Total Memory Deallocations: {}\n"
        "Peak Memory Usage: {}\n"
        "Current Memory Usage: {}\n"
        "Log Size: {} bytes\n"
        "==============================\n",
        total_vulkan_calls.load(std::memory_order_relaxed),
        total_allocations.load(std::memory_order_relaxed),
        total_deallocations.load(std::memory_order_relaxed),
        FormatMemorySize(peak_allocated_bytes.load(std::memory_order_relaxed)),
        FormatMemorySize(current_allocated_bytes.load(std::memory_order_relaxed)),
        bytes_written.load(std::memory_order_relaxed));

    // Close file
    {
        std::lock_guard file_lock(file_mutex);
        if (gpu_log_file) {
            if (gpu_log_file->IsOpen()) {
                bytes_written.fetch_add(static_cast<u64>(gpu_log_file->WriteString(stats)),
                                        std::memory_order_relaxed);
                gpu_log_file->Flush();
                gpu_log_file->Close();
            }
            gpu_log_file.reset();
        }
    }

    // Note: Crash handler is NOT shut down here - it remains active throughout app lifetime
    // It will be shut down when EmulationSession is destroyed

    if (log_message) {
        LOG_INFO(Render_Vulkan, "[GPU Logging] Shutdown complete");
    }
}

void GPULogger::LogVulkanCall(const std::string& call_name, const std::string& params,
                              int result) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (!track_vulkan_calls.load(std::memory_order_relaxed)) {
        return;
    }

    // Only log all calls in Verbose or All mode
    if (level != LogLevel::Verbose && level != LogLevel::All) {
        // In Standard mode, only log important calls
        if (call_name.find("vkCmd") == std::string::npos &&
            call_name.find("vkCreate") == std::string::npos &&
            call_name.find("vkDestroy") == std::string::npos) {
            return;
        }
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    const auto thread_id = static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // Add to ring buffer
    {
        std::lock_guard lock(ring_buffer_mutex);
        const size_t capacity = call_ring_buffer.size();
        if (capacity > 0) {
            if (ring_buffer_index >= capacity) {
                ring_buffer_index = 0;
            }
            call_ring_buffer[ring_buffer_index] = {
                .timestamp = timestamp,
                .call_name = call_name,
                .parameters = params,
                .result = result,
                .thread_id = thread_id,
            };
            ring_buffer_index = (ring_buffer_index + 1) % capacity;
        }
        total_vulkan_calls.fetch_add(1, std::memory_order_relaxed);
    }

    // Log to file
    const auto log_entry =
        fmt::format("[{}] [Vulkan] [Thread:{}] {}({}) -> {}\n", FormatTimestamp(timestamp),
                    thread_id, call_name, params, result);
    WriteToLog(log_entry);
}

void GPULogger::LogMemoryAllocation(uintptr_t memory, u64 size, u32 memory_flags) {
    if (!initialized.load(std::memory_order_acquire) ||
        current_level.load(std::memory_order_acquire) == LogLevel::Off) {
        return;
    }

    if (!track_memory.load(std::memory_order_relaxed)) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const bool is_device_local = (memory_flags & 0x1) != 0;  // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    const bool is_host_visible = (memory_flags & 0x2) != 0;  // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT

    {
        std::lock_guard lock(memory_mutex);
        memory_allocations[memory] = {
            .memory_handle = memory,
            .size = size,
            .memory_flags = memory_flags,
            .timestamp = timestamp,
            .is_device_local = is_device_local,
            .is_host_visible = is_host_visible,
        };

        total_allocations.fetch_add(1, std::memory_order_relaxed);
        const u64 new_current =
            current_allocated_bytes.fetch_add(size, std::memory_order_relaxed) + size;
        if (new_current > peak_allocated_bytes.load(std::memory_order_relaxed)) {
            peak_allocated_bytes.store(new_current, std::memory_order_relaxed);
        }
    }

    const auto log_entry = fmt::format(
        "[{}] [Memory] Allocated {} at {:#x} (Device:{}, Host:{})\n", FormatTimestamp(timestamp),
        FormatMemorySize(size), memory, is_device_local ? "Yes" : "No",
        is_host_visible ? "Yes" : "No");
    WriteToLog(log_entry);
}

void GPULogger::LogMemoryDeallocation(uintptr_t memory) {
    if (!initialized.load(std::memory_order_acquire) ||
        current_level.load(std::memory_order_acquire) == LogLevel::Off) {
        return;
    }

    if (!track_memory.load(std::memory_order_relaxed)) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    u64 size = 0;
    {
        std::lock_guard lock(memory_mutex);
        auto it = memory_allocations.find(memory);
        if (it != memory_allocations.end()) {
            size = it->second.size;
            const u64 prev_current = current_allocated_bytes.load(std::memory_order_relaxed);
            current_allocated_bytes.store(prev_current >= size ? prev_current - size : 0,
                                          std::memory_order_relaxed);
            memory_allocations.erase(it);
            total_deallocations.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (size > 0) {
        const auto log_entry =
            fmt::format("[{}] [Memory] Deallocated {} at {:#x}\n", FormatTimestamp(timestamp),
                        FormatMemorySize(size), memory);
        WriteToLog(log_entry);
    }
}

void GPULogger::LogShaderCompilation(const std::string& shader_name,
                                     const std::string& shader_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry = fmt::format("[{}] [Shader] Compiled: {} ({})\n",
                                       FormatTimestamp(timestamp), shader_name, shader_info);
    WriteToLog(log_entry);
}

bool IsActive() noexcept {
    return Settings::values.gpu_log_level.GetValue() != Settings::GpuLogLevel::Off;
}

void DumpSpirvShader(u64 shader_hash, std::span<const u32> spirv_code) {
    if (spirv_code.empty()) {
        return;
    }

    using namespace Common::FS;
    const auto& dump_dir = GetEdenPath(EdenPath::DumpDir);

    // Ensure DumpDir exists once. CreateDir is idempotent, so guarded to skip the syscall.
    static std::once_flag dump_dir_flag;
    std::call_once(dump_dir_flag, [&dump_dir]() {
        [[maybe_unused]] const bool created = CreateDir(dump_dir);
    });

    const auto shader_path = dump_dir / fmt::format("{:016x}_{:016x}.spv",
                                                    Settings::GetCurrentProgramID(), shader_hash);
    Common::FS::IOFile shader_file(shader_path, FileAccessMode::Write, FileType::BinaryFile);
    if (!shader_file.IsOpen()) {
        LOG_WARNING(Render_Vulkan, "[Shader Dump] Failed to open {}", shader_path.string());
        return;
    }
    static_cast<void>(shader_file.WriteSpan(spirv_code));
}

void GPULogger::LogPipelineStateChange(const std::string& state_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    // Store pipeline state for crash dumps
    {
        std::lock_guard lock(state_mutex);
        stored_pipeline_state = state_info;
    }

    if (level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry =
        fmt::format("[{}] [Pipeline] State change: {}\n", FormatTimestamp(timestamp), state_info);
    WriteToLog(log_entry);
}

void GPULogger::LogDriverDebugInfo(const std::string& debug_info) {
    if (!initialized.load(std::memory_order_acquire) ||
        current_level.load(std::memory_order_acquire) == LogLevel::Off) {
        return;
    }

    // Store driver debug info for crash dumps
    {
        std::lock_guard lock(state_mutex);
        stored_driver_debug_info = debug_info;
    }

    if (!capture_driver_debug.load(std::memory_order_relaxed)) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry =
        fmt::format("[{}] [Driver] {}\n", FormatTimestamp(timestamp), debug_info);
    WriteToLog(log_entry);
}

void GPULogger::LogExtensionUsage(const std::string& extension_name, const std::string& function_name) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    bool is_first_use = false;
    {
        std::lock_guard lock(extension_mutex);
        auto [iter, inserted] = used_extensions.insert(extension_name);
        is_first_use = inserted;
    }

    if (is_first_use) {
        const auto log_entry = fmt::format("[{}] [Extension] First use of {} in {}\n",
            FormatTimestamp(timestamp), extension_name, function_name);
        WriteToLog(log_entry);
        LOG_INFO(Render_Vulkan, "[GPU Logging] First use of extension {} in {}",
                 extension_name, function_name);
    } else if (level >= LogLevel::Verbose) {
        const auto log_entry = fmt::format("[{}] [Extension] {} used in {}\n",
            FormatTimestamp(timestamp), extension_name, function_name);
        WriteToLog(log_entry);
    }
}

void GPULogger::LogRenderPassBegin(const std::string& render_pass_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (!track_vulkan_calls.load(std::memory_order_relaxed) && level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry = fmt::format("[{}] [RenderPass] Begin: {}\n",
        FormatTimestamp(timestamp), render_pass_info);
    WriteToLog(log_entry);
}

void GPULogger::LogRenderPassEnd() {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (!track_vulkan_calls.load(std::memory_order_relaxed) && level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry = fmt::format("[{}] [RenderPass] End\n", FormatTimestamp(timestamp));
    WriteToLog(log_entry);
}

void GPULogger::LogPipelineBind(bool is_compute, const std::string& pipeline_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (!track_vulkan_calls.load(std::memory_order_relaxed) && level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const char* pipeline_type = is_compute ? "Compute" : "Graphics";
    const auto log_entry = fmt::format("[{}] [Pipeline] Bind {} pipeline: {}\n",
        FormatTimestamp(timestamp), pipeline_type, pipeline_info);
    WriteToLog(log_entry);
}

void GPULogger::LogDescriptorSetBind(const std::string& descriptor_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry = fmt::format("[{}] [Descriptor] Bind: {}\n",
        FormatTimestamp(timestamp), descriptor_info);
    WriteToLog(log_entry);
}

void GPULogger::LogPipelineBarrier(const std::string& barrier_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry = fmt::format("[{}] [Barrier] {}\n",
        FormatTimestamp(timestamp), barrier_info);
    WriteToLog(log_entry);
}

void GPULogger::LogImageOperation(const std::string& operation, const std::string& image_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (!track_vulkan_calls.load(std::memory_order_relaxed) && level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry = fmt::format("[{}] [Image] {}: {}\n",
        FormatTimestamp(timestamp), operation, image_info);
    WriteToLog(log_entry);
}

void GPULogger::LogClearOperation(const std::string& clear_info) {
    const LogLevel level = current_level.load(std::memory_order_acquire);
    if (!initialized.load(std::memory_order_acquire) || level == LogLevel::Off) {
        return;
    }

    if (!track_vulkan_calls.load(std::memory_order_relaxed) && level < LogLevel::Verbose) {
        return;
    }

    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    const auto log_entry = fmt::format("[{}] [Clear] {}\n",
        FormatTimestamp(timestamp), clear_info);
    WriteToLog(log_entry);
}

GPUStateSnapshot GPULogger::GetCurrentSnapshot() {
    GPUStateSnapshot snapshot;
    snapshot.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    snapshot.driver_type = detected_driver.load(std::memory_order_acquire);

    // Capture recent Vulkan calls
    {
        std::lock_guard lock(ring_buffer_mutex);
        const size_t capacity = call_ring_buffer.size();
        snapshot.recent_calls.reserve(capacity);
        if (capacity > 0) {
            const size_t start = ring_buffer_index < capacity ? ring_buffer_index : 0;

            // Copy from current position to end
            for (size_t i = start; i < capacity; ++i) {
                if (!call_ring_buffer[i].call_name.empty()) {
                    snapshot.recent_calls.push_back(call_ring_buffer[i]);
                }
            }

            // Copy from beginning to current position
            for (size_t i = 0; i < start; ++i) {
                if (!call_ring_buffer[i].call_name.empty()) {
                    snapshot.recent_calls.push_back(call_ring_buffer[i]);
                }
            }
        }
    }

    // Capture memory status
    {
        std::lock_guard lock(memory_mutex);
        snapshot.memory_status = fmt::format(
            "Total Allocations: {}\n"
            "Current Usage: {}\n"
            "Peak Usage: {}\n"
            "Active Allocations: {}\n",
            total_allocations.load(std::memory_order_relaxed),
            FormatMemorySize(current_allocated_bytes.load(std::memory_order_relaxed)),
            FormatMemorySize(peak_allocated_bytes.load(std::memory_order_relaxed)),
            memory_allocations.size());
    }

    // Capture stored pipeline and driver debug info
    {
        std::lock_guard lock(state_mutex);
        snapshot.pipeline_state = stored_pipeline_state.empty() ?
            "No pipeline state logged yet" : stored_pipeline_state;
        snapshot.driver_debug_info = stored_driver_debug_info.empty() ?
            "No driver debug info logged yet" : stored_driver_debug_info;
    }

    return snapshot;
}

void GPULogger::DumpStateToFile(const std::string& crash_reason) {
    using namespace Common::FS;
    const auto& log_dir = GetEdenPath(EdenPath::LogDir);
    const auto crashes_dir = log_dir / "gpu_crashes";
    [[maybe_unused]] const bool crashes_dir_created = CreateDir(crashes_dir);

    // Generate crash dump filename with timestamp
    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    const auto crash_dump_path = crashes_dir / fmt::format("crash_{}.gpu-dump", timestamp);

    auto crash_file =
        std::make_unique<Common::FS::IOFile>(crash_dump_path, FileAccessMode::Write, FileType::TextFile);

    if (!crash_file->IsOpen()) {
        LOG_ERROR(Render_Vulkan, "[GPU Logging] Failed to create crash dump file");
        return;
    }

    auto snapshot = GetCurrentSnapshot();

    const char* driver_name = "Unknown";
    switch (snapshot.driver_type) {
    case DriverType::Turnip:
        driver_name = "Turnip (Mesa Freedreno)";
        break;
    case DriverType::Qualcomm:
        driver_name = "Qualcomm Proprietary";
        break;
    default:
        driver_name = "Unknown";
        break;
    }

    // Write crash dump header
    const auto header = fmt::format(
        "=== GPU CRASH DUMP ===\n"
        "Timestamp: {}\n"
        "Reason: {}\n"
        "Driver: {}\n"
        "\n",
        FormatTimestamp(snapshot.timestamp), crash_reason, driver_name);
    static_cast<void>(crash_file->WriteString(header));

    // Write recent Vulkan calls
    static_cast<void>(crash_file->WriteString(fmt::format("=== RECENT VULKAN API CALLS (Last {}) ===\n",
                                        snapshot.recent_calls.size())));
    for (const auto& call : snapshot.recent_calls) {
        const auto call_str =
            fmt::format("[{}] [Thread:{}] {}({}) -> {}\n", FormatTimestamp(call.timestamp),
                        call.thread_id, call.call_name, call.parameters, call.result);
        static_cast<void>(crash_file->WriteString(call_str));
    }
    static_cast<void>(crash_file->WriteString("\n"));

    // Write memory status
    static_cast<void>(crash_file->WriteString("=== MEMORY STATUS ===\n"));
    static_cast<void>(crash_file->WriteString(snapshot.memory_status));
    static_cast<void>(crash_file->WriteString("\n"));

    // Write pipeline state
    static_cast<void>(crash_file->WriteString("=== PIPELINE STATE ===\n"));
    static_cast<void>(crash_file->WriteString(snapshot.pipeline_state));
    static_cast<void>(crash_file->WriteString("\n"));

    // Write driver debug info
    static_cast<void>(crash_file->WriteString("=== DRIVER DEBUG INFO ===\n"));
    static_cast<void>(crash_file->WriteString(snapshot.driver_debug_info));
    static_cast<void>(crash_file->WriteString("\n"));

    crash_file->Flush();
    crash_file->Close();

    LOG_CRITICAL(Render_Vulkan, "[GPU Logging] Crash dump written to: {}",
                 crash_dump_path.string());
}

void GPULogger::SetLogLevel(LogLevel level) {
    current_level.store(level, std::memory_order_release);
}

void GPULogger::EnableVulkanCallTracking(bool enabled) {
    track_vulkan_calls.store(enabled, std::memory_order_relaxed);
}

void GPULogger::EnableMemoryTracking(bool enabled) {
    track_memory.store(enabled, std::memory_order_relaxed);
}

void GPULogger::EnableDriverDebugInfo(bool enabled) {
    capture_driver_debug.store(enabled, std::memory_order_relaxed);
}

void GPULogger::SetRingBufferSize(size_t entries) {
    std::lock_guard lock(ring_buffer_mutex);
    ring_buffer_size = (std::max)(entries, size_t{1});
    call_ring_buffer.assign(ring_buffer_size, VulkanCallEntry{});
    ring_buffer_index = 0;
}

LogLevel GPULogger::GetLogLevel() const {
    return current_level.load(std::memory_order_acquire);
}

DriverType GPULogger::GetDriverType() const {
    return detected_driver.load(std::memory_order_acquire);
}

std::string GPULogger::GetStatistics() const {
    std::lock_guard lock(memory_mutex);
    return fmt::format(
        "Vulkan Calls: {}, Allocations: {}, Deallocations: {}, "
        "Current Memory: {}, Peak Memory: {}",
        total_vulkan_calls.load(std::memory_order_relaxed),
        total_allocations.load(std::memory_order_relaxed),
        total_deallocations.load(std::memory_order_relaxed),
        FormatMemorySize(current_allocated_bytes.load(std::memory_order_relaxed)),
        FormatMemorySize(peak_allocated_bytes.load(std::memory_order_relaxed)));
}

bool GPULogger::IsInitialized() const {
    return initialized.load(std::memory_order_acquire);
}

void GPULogger::WriteToLog(const std::string& message) {
    std::lock_guard lock(file_mutex);
    if (!gpu_log_file || !gpu_log_file->IsOpen()) {
        return;
    }

    const u64 prev_written = bytes_written.load(std::memory_order_relaxed);
    const u64 wrote = static_cast<u64>(gpu_log_file->WriteString(message));
    const u64 new_written = prev_written + wrote;
    bytes_written.store(new_written, std::memory_order_relaxed);

    // Flush on errors or if we've written a lot
    using namespace Common::Literals;
    if (new_written / 1_MiB > prev_written / 1_MiB) {
        gpu_log_file->Flush();
    }
}

std::string GPULogger::FormatTimestamp(std::chrono::microseconds timestamp) const {
    const auto seconds = timestamp.count() / 1000000;
    const auto microseconds = timestamp.count() % 1000000;
    return fmt::format("{:4d}.{:06d}", seconds, microseconds);
}

std::string GPULogger::FormatMemorySize(u64 bytes) const {
    using namespace Common::Literals;
    if (bytes >= 1_GiB) {
        return fmt::format("{:.2f} GiB", static_cast<double>(bytes) / (1_GiB));
    } else if (bytes >= 1_MiB) {
        return fmt::format("{:.2f} MiB", static_cast<double>(bytes) / (1_MiB));
    } else if (bytes >= 1_KiB) {
        return fmt::format("{:.2f} KiB", static_cast<double>(bytes) / (1_KiB));
    } else {
        return fmt::format("{} B", bytes);
    }
}

} // namespace GPU::Logging
