# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    core/nx2_frame_interval.cpp
    core/save_data_lifecycle.cpp
    video_core/astc.cpp
    video_core/etc2.cpp
    video_core/upload_batch.cpp
    video_core/texture_swizzle.cpp
    core/nx2_oot_time.cpp
    video_core/unbacked_image_policy.cpp
    core/builtin_patches.cpp
    core/gpu_dirty_memory_manager.cpp
    video_core/gpu_timestamp.cpp
    video_core/maxwell_fast_method.cpp
    video_core/split_write_tracker.cpp
    video_core/list_writer_trace.cpp
    video_core/list_copy_demand.cpp
    video_core/buffer_dependency_trace.cpp
    video_core/nx2_written_pages.cpp
    core/system_test_context.h
    video_core/ampere_logged_numeric.cpp
    video_core/mesh_fallback.cpp
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/cpu_layout.cpp
    common/event.cpp
    common/fibers.cpp
    common/game_presets.cpp
    common/host_memory.cpp
    common/page_table.cpp
    common/progress_counter.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/range_set_subtract.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/slot_vector.cpp
    common/unique_function.cpp
    common/zstd_compression.cpp
    core/cmif_out_buffer.cpp
    core/content_provider_init.cpp
    core/core_timing.cpp
    core/kernel_finish_termination.cpp
    core/key_code_map.cpp
    core/launch_params.cpp
    core/ldn.cpp
    core/loader_file_formats.cpp
    core/memory_range.cpp
    core/nso_loader.cpp
    core/internal_network/socket_proxy.cpp
    core/nup_writer.cpp
    core/nx2_debug_server.cpp
    core/nx2_movie_pacing.cpp
    core/nx2_gpu_memory_registry.cpp
    core/nx2_gsv_bounds.cpp
    core/nx2_guest_ranges.cpp
    core/nx2_session_state.cpp
    core/platform_profile.cpp
    core/platform_sizing.cpp
    core/priority_queue.cpp
    core/process_load.cpp
    core/shutdown_ordering.cpp
    core/internal_network/network.cpp
    video_core/frame_dump_schedule.cpp
    video_core/framebuffer_limits.cpp
    video_core/memory_tracker.cpp
    video_core/nx2_perf.cpp
    video_core/nx2_perf_draw_binds.cpp
    video_core/nx2_perf_texture_writes.cpp
    video_core/nx2_perf_native_qmd.cpp
    video_core/nx2_perf_query_callback.cpp
    video_core/nx2_perf_switches.cpp
    video_core/memory_manager_runs.cpp
    video_core/memory_manager_page_table.cpp
    video_core/nx2_concurrency.cpp
    video_core/nx2_macro_context.cpp
    video_core/buffer_download_policy.cpp
    video_core/nx2_sparse_residency.cpp
    video_core/spirv_storage_alignment.cpp
    video_core/pipeline_cache_dir.cpp
    video_core/pipeline_optimization.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/ampere_depth_compare.cpp
    video_core/shader_bench.cpp
    video_core/shader_precache.cpp
    video_core/shader_precache_identity.cpp
    video_core/h264_packet.cpp
    video_core/vertex_repack.cpp
    video_core/color_target_redraw.cpp
    video_core/draw_skip_policy.cpp
    video_core/virtual_range_cache.cpp
    video_core/query_copy.cpp
    video_core/ampere_direct_spirv.cpp
    video_core/ampere_lazy_lowering.cpp
    video_core/channel_bridge_parity.cpp
    video_core/ampere_lowering.cpp
    video_core/ampere_program.cpp
    video_core/spirv_software_matrix.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_i2f.cpp
    video_core/ampere_gradient.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_compute_lowering.cpp
    video_core/native_macro_fifo.cpp
    video_core/native_image_matching.cpp
    video_core/texture_cache_budget.cpp
    video_core/usage_tracker.cpp
    video_core/native_swizzle.cpp
    video_core/image_allocation_pool.cpp
    video_core/plain_unswizzle.cpp
    video_core/uniform_stream_reuse.cpp
    video_core/shader_invalidation.cpp
    video_core/descriptor_table.cpp
    video_core/global_address_tracking.cpp
    video_core/global_atomic_routing.cpp
    video_core/nvn_storage_descriptor.cpp
    video_core/ampere_f2i.cpp
    video_core/ampere_ldc.cpp
    video_core/ampere_vote.cpp
    video_core/shader_attributes.cpp
    video_core/indirect_reset.cpp
    video_core/ocarina_upscale.cpp
    video_core/pending_download.cpp
    video_core/shader_ir_edges.cpp
    video_core/syncpoint_manager.cpp
    video_core/texture_cache_lifetime.cpp
    video_core/texture_clear_covers.cpp
    video_core/vulkan_device_version.cpp
    video_core/vulkan_static_cache.cpp
    audio_core/opus_decode.cpp
    audio_core/sink_stream_silent.cpp
    core/memory_span.cpp
    frontend_common/config_persistence.cpp
    frontend_common/play_time.cpp
    input_common/calibration_configuration_job.cpp
    network/packet_bounds.cpp
    network/room_information.cpp
)

if (NOT ANDROID)
    target_sources(tests PRIVATE
        input_common/switch2_auto_pair.cpp
        input_common/switch2_protocol.cpp)
endif()

create_target_directory_groups(tests)

if (ENABLE_UPDATE_CHECKER)
    target_sources(tests PRIVATE
        common/update_checker.cpp
        common/update_installer.cpp)
endif()

target_link_libraries(tests PRIVATE audio_core common core frontend_common input_common video_core)
if (TARGET eglantine_rs)
    target_link_libraries(tests PRIVATE eglantine_rs)
endif()

if (ENABLE_MCP_SERVER)
    target_sources(tests PRIVATE mcp/protocol.cpp)
    target_link_libraries(tests PRIVATE mcp)
endif()
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)
add_test(NAME tests_ampere_direct_sm86 COMMAND tests "[ampere-lazy-lowering]")
set_tests_properties(tests_ampere_direct_sm86 PROPERTIES ENVIRONMENT "EDEN_NX2_DIRECT_SM86=1")
add_test(NAME tests_nx2_perf_verify COMMAND tests "[nx2_perf]")
set_tests_properties(tests_nx2_perf_verify PROPERTIES ENVIRONMENT "EDEN_NX2_PERF_VERIFY=1")

# needed for vma
if(NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
