// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EGL_SPIRV_MAX_CBUFS 18u
#define EGL_SPIRV_MAX_SSBOS 64u

typedef struct EglAmpereInstruction {
    uint32_t words[4];
} EglAmpereInstruction;

typedef enum EglSpirvStage {
    EGL_SPIRV_STAGE_VERTEX_B = 0,
    EGL_SPIRV_STAGE_TESS_CONTROL = 1,
    EGL_SPIRV_STAGE_TESS_EVAL = 2,
    EGL_SPIRV_STAGE_GEOMETRY = 3,
    EGL_SPIRV_STAGE_FRAGMENT = 4,
    EGL_SPIRV_STAGE_COMPUTE = 5,
    EGL_SPIRV_STAGE_VERTEX_A = 6,
    EGL_SPIRV_STAGE_TASK = 7,
    EGL_SPIRV_STAGE_MESH = 8,
} EglSpirvStage;

/// Cooperative matrix tile shape and register-to-element lane mapping (Phase 2B calibration).
typedef struct EglSpirvCoopLayout {
    uint8_t supported;      /* Non-zero when SPV_KHR_cooperative_matrix is calibrated */
    uint8_t use_fp16_accum; /* Non-zero for FP16 accumulator (HMMA.16816.F16), 0 for FP32 */
    uint8_t _pad[2];
    uint32_t tile_m;        /* Matrix M dimension (e.g. 16) */
    uint32_t tile_n;        /* Matrix N dimension (e.g. 8 on NVIDIA, 16 on AMD/Intel) */
    uint32_t tile_k;        /* Matrix K dimension (e.g. 16) */
    uint32_t subgroup_size; /* Calibrated subgroup size (e.g. 32 or 64) */
    uint32_t a_word_map[4]; /* Guest R[a..a+3] -> MatrixAKHR word slot map (e.g. {0, 2, 1, 3}) */
    uint32_t b_word_map[2]; /* Guest R[b..b+1] -> MatrixBKHR word slot map (e.g. {0, 1}) */
    uint32_t c_word_map[2]; /* Guest R[c..c+1] -> MatrixAccumulatorKHR input map */
    uint32_t d_word_map[2]; /* MatrixAccumulatorKHR output -> Guest R[d..d+1] map */
} EglSpirvCoopLayout;

typedef struct EglSpirvProfile {
    uint32_t supported_spirv; /* Must be >= 0x00010600 (SPIR-V 1.6) for direct emission */
    uint32_t max_subgroup_size;
    uint32_t supported_subgroup_stages;
    uint32_t max_user_clip_distances;
    uint64_t min_ssbo_alignment;
    uint8_t unified_descriptor_binding;
    uint8_t support_descriptor_aliasing;
    uint8_t support_int8;
    uint8_t support_uniform_and_storage_buffer_8bit;
    uint8_t support_storage_buffer_8bit;
    uint8_t support_int16;
    uint8_t support_uniform_and_storage_buffer_16bit;
    uint8_t support_storage_buffer_16bit;
    uint8_t support_int64;
    uint8_t support_float16;
    uint8_t support_float64;
    uint8_t support_float_controls;
    uint8_t support_explicit_workgroup_layout;
    uint8_t support_workgroup_layout_8bit_access;
    uint8_t support_workgroup_layout_16bit_access;
    uint8_t support_vote;
    uint8_t support_shuffle_relative;
    uint8_t support_quad_shuffles;
    uint8_t support_demote_to_helper_invocation;
    uint8_t support_int64_atomics;
    uint8_t support_shared_int64_atomics;
    uint8_t support_typeless_image_loads;
    uint8_t warp_size_potentially_larger_than_guest;
    uint8_t lower_left_origin_mode;
    uint8_t has_broken_spirv_clamp;
    uint8_t has_broken_fp16_float_controls;
    uint8_t has_broken_fp32_denorm_flush;
    uint8_t _pad[5];
    EglSpirvCoopLayout coop_layout;
    uint32_t _pad_end;
} EglSpirvProfile;

/// Mirrors `Shader::Backend::Bindings` in `src/shader_recompiler/backend/bindings.h`.
typedef struct EglSpirvBindings {
    uint32_t unified;
    uint32_t uniform_buffer;
    uint32_t storage_buffer;
    uint32_t texture;
    uint32_t image;
    uint32_t texture_scaling_index;
    uint32_t image_scaling_index;
} EglSpirvBindings;

typedef struct EglSpirvCbufEntry {
    uint32_t index;
    uint32_t offset;
    uint32_t value;
} EglSpirvCbufEntry;

typedef struct EglSpirvTextureTypeEntry {
    uint32_t handle;
    uint32_t texture_type; /* Shader::TextureType */
} EglSpirvTextureTypeEntry;

typedef struct EglSpirvTextureFormatEntry {
    uint32_t handle;
    uint32_t pixel_format; /* Shader::TexturePixelFormat */
} EglSpirvTextureFormatEntry;

typedef struct EglSpirvStoragePtrEntry {
    uint64_t key;
    uint8_t is_mapped;
    uint8_t _pad[7];
} EglSpirvStoragePtrEntry;

typedef struct EglSpirvCbufReplaceEntry {
    uint32_t bank;
    uint32_t offset;
    uint32_t replace_constant; /* Shader::ReplaceConstant */
} EglSpirvCbufReplaceEntry;

typedef struct EglSpirvCompileInput {
    uint32_t stage;              /* EglSpirvStage / Shader::Stage */
    uint32_t texture_bound;      /* TextureBoundBuffer(), ~0U when unset */
    uint64_t program_key;        /* Shader::Ampere::NativeProgramKey */
    uint64_t program_header[10]; /* Shader::ProgramHeader (80 bytes) */
    const EglAmpereInstruction* code;
    size_t code_count;
    uint32_t workgroup_size[3];
    uint32_t local_memory_size;
    uint32_t shared_memory_size;
    uint8_t is_task_shader;
    uint8_t is_mesh_shader;
    uint8_t native_tensor;
    uint8_t native_compact_tensor;
    uint8_t has_hle_engine_state;
    uint8_t mesh_as_compute;
    uint8_t _pad0[2];
    uint32_t viewport_transform_state;
    uint32_t mesh_max_vertices;
    uint32_t mesh_max_primitives;
    uint32_t mesh_topology;
    uint32_t mesh_payload_words;
    uint32_t _pad1;

    const EglSpirvCbufEntry* cbufs;
    size_t cbuf_count;
    const EglSpirvTextureTypeEntry* texture_types;
    size_t texture_type_count;
    const EglSpirvTextureFormatEntry* texture_formats;
    size_t texture_format_count;
    const EglSpirvStoragePtrEntry* storage_ptrs;
    size_t storage_ptr_count;
    const EglSpirvCbufReplaceEntry* replacements;
    size_t replacement_count;

    EglSpirvProfile profile;
    EglSpirvBindings bindings;
    uint32_t _pad2;
} EglSpirvCompileInput;

typedef struct EglSpirvCbufDescriptor {
    uint32_t index;
    uint32_t count;
} EglSpirvCbufDescriptor;

typedef struct EglSpirvSsboDescriptor {
    uint32_t cbuf_index;
    uint32_t cbuf_offset;
    uint32_t count;
    uint32_t indirect_offset; /* ~0U when direct */
    uint8_t is_written;
    uint8_t _pad[3];
} EglSpirvSsboDescriptor;

typedef struct EglSpirvTextureBufferDescriptor {
    uint32_t cbuf_index;
    uint32_t cbuf_offset;
    uint32_t shift_left;
    uint32_t secondary_cbuf_index;
    uint32_t secondary_cbuf_offset;
    uint32_t secondary_shift_left;
    uint32_t count;
    uint32_t size_shift;
    uint32_t handle_program; /* ~0U when constant-buffer handle */
    uint8_t has_secondary;
    uint8_t _pad[3];
} EglSpirvTextureBufferDescriptor;

typedef struct EglSpirvImageBufferDescriptor {
    uint32_t format; /* Shader::ImageFormat */
    uint32_t cbuf_index;
    uint32_t cbuf_offset;
    uint32_t count;
    uint32_t size_shift;
    uint32_t handle_program;
    uint8_t is_written;
    uint8_t is_read;
    uint8_t is_integer;
    uint8_t _pad;
} EglSpirvImageBufferDescriptor;

typedef struct EglSpirvTextureDescriptor {
    uint32_t type; /* Shader::TextureType */
    uint32_t cbuf_index;
    uint32_t cbuf_offset;
    uint32_t shift_left;
    uint32_t secondary_cbuf_index;
    uint32_t secondary_cbuf_offset;
    uint32_t secondary_shift_left;
    uint32_t count;
    uint32_t size_shift;
    uint32_t handle_program;
    uint8_t is_depth;
    uint8_t is_multisample;
    uint8_t is_integer;
    uint8_t has_secondary;
} EglSpirvTextureDescriptor;

typedef struct EglSpirvImageDescriptor {
    uint32_t type;   /* Shader::TextureType */
    uint32_t format; /* Shader::ImageFormat */
    uint32_t cbuf_index;
    uint32_t cbuf_offset;
    uint32_t count;
    uint32_t size_shift;
    uint32_t handle_program;
    uint8_t is_written;
    uint8_t is_read;
    uint8_t is_integer;
    uint8_t _pad;
} EglSpirvImageDescriptor;

/// Reflected pipeline/descriptor summary mirroring `Shader::Info`.
typedef struct EglShaderInfoSummary {
    uint32_t constant_buffer_mask;
    uint32_t constant_buffer_used_sizes[EGL_SPIRV_MAX_CBUFS];
    uint32_t used_constant_buffer_types;
    uint32_t used_storage_buffer_types;
    uint32_t used_indirect_cbuf_types;
    uint32_t nvn_buffer_base;
    uint32_t nvn_buffer_used_mask;
    uint32_t used_clip_distances;
    uint32_t stores_frag_color_mask;
    uint8_t frag_color_components[8];

    uint8_t uses_workgroup_id;
    uint8_t uses_local_invocation_id;
    uint8_t uses_invocation_id;
    uint8_t uses_invocation_info;
    uint8_t uses_sample_id;
    uint8_t uses_is_helper_invocation;
    uint8_t uses_subgroup_invocation_id;
    uint8_t uses_subgroup_shuffles;
    uint8_t uses_quad_shuffles;
    uint8_t uses_subgroup_vote;
    uint8_t uses_subgroup_mask;
    uint8_t uses_fswzadd;
    uint8_t uses_fp16;
    uint8_t uses_fp64;
    uint8_t uses_fp16_denorms_flush;
    uint8_t uses_fp16_denorms_preserve;
    uint8_t uses_fp32_denorms_flush;
    uint8_t uses_fp32_denorms_preserve;
    uint8_t uses_int8;
    uint8_t uses_int16;
    uint8_t uses_int64;
    uint8_t uses_local_memory;
    uint8_t uses_global_memory;
    uint8_t stores_global_memory;
    uint8_t requires_coherent_global_memory;
    uint8_t requires_coherent_image_memory;
    uint8_t uses_cooperative_matrix;
    uint8_t requires_full_subgroups;
    uint8_t native_global_bounds;
    uint8_t uses_cbuf_indirect;
    uint8_t uses_rescaling_uniform;
    uint8_t uses_render_area;
    uint8_t stores_frag_depth;
    uint8_t stores_sample_mask;
    uint8_t uses_demote_to_helper_invocation;
    uint8_t uses_derivatives;
    uint8_t uses_typeless_image_reads;
    uint8_t uses_typeless_image_writes;
    uint8_t uses_shared_increment;
    uint8_t uses_shared_decrement;
    uint8_t uses_global_increment;
    uint8_t uses_global_decrement;
    uint8_t uses_atomic_f32_add;
    uint8_t uses_atomic_s32_min;
    uint8_t uses_atomic_s32_max;
    uint8_t uses_int64_bit_atomics;
    uint8_t is_task;
    uint8_t is_mesh;
    uint8_t uses_push_constants;
    uint8_t uses_image_1d;
    uint8_t uses_sampled_1d;
    uint8_t uses_image_buffers;
    uint8_t uses_atomic_image_u32;
    uint8_t removed_conditional_barrier;
    uint8_t uses_shadow_lod;
    uint8_t _pad_flags[1];

    EglSpirvCbufDescriptor constant_buffer_descriptors[EGL_SPIRV_MAX_CBUFS];
    size_t constant_buffer_descriptor_count;
    EglSpirvSsboDescriptor storage_buffer_descriptors[EGL_SPIRV_MAX_SSBOS];
    size_t storage_buffer_descriptor_count;

    EglSpirvTextureBufferDescriptor* texture_buffer_descriptors;
    size_t texture_buffer_descriptor_count;
    EglSpirvImageBufferDescriptor* image_buffer_descriptors;
    size_t image_buffer_descriptor_count;
    EglSpirvTextureDescriptor* texture_descriptors;
    size_t texture_descriptor_count;
    EglSpirvImageDescriptor* image_descriptors;
    size_t image_descriptor_count;
} EglShaderInfoSummary;

typedef struct EglSpirvCompileOutput {
    uint32_t* spirv_words;
    size_t word_count;
    uint8_t fallback_to_maxwell;
    uint8_t _pad[3];
    uint32_t fallback_offset;
    uint32_t fallback_opcode;
    uint32_t error_offset;
    char* error;
    size_t error_len;
    EglSpirvBindings updated_bindings;
    uint32_t _pad_bindings;
    EglShaderInfoSummary info;
} EglSpirvCompileOutput;

/// Compiles one SM86 shader snapshot directly to SPIR-V 1.6.
/// Returns 0 on valid invocation (check `out_output->fallback_to_maxwell` and `out_output->error_len`),
/// or non-zero on invalid arguments / internal panic.
int32_t egl_ampere_spirv_compile(const EglSpirvCompileInput* input,
                                 EglSpirvCompileOutput* out_output);

/// Compiles `count` independent shader snapshots in parallel via `std::thread::scope`.
/// Returns 0 on valid invocation, or non-zero on invalid arguments / internal panic.
int32_t egl_ampere_spirv_compile_batch(const EglSpirvCompileInput* inputs,
                                       size_t count,
                                       EglSpirvCompileOutput* outputs);

/// Idempotent: frees owned buffers inside `output` and zeroes all fields.
void egl_ampere_spirv_free(EglSpirvCompileOutput* output);

#ifdef __cplusplus
} // extern "C"
#endif
