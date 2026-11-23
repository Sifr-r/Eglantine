// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

//! C99 / C++20 ABI layer matching `rust/include/eglantine_ampere_spirv.h`.

use std::ffi::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use eglantine_ampere_rs::ffi::EglAmpereInstruction;
use eglantine_ampere_rs::Instruction;

use crate::coop_matrix::CoopConfig;
use crate::reflect::{
    Bindings, CbufDescriptor, ImageBufferDescriptor, ImageDescriptor, Profile, ShaderInfo,
    SnapshotEnv, SsboDescriptor, Stage, TextureBufferDescriptor, TextureDescriptor, MAX_CBUFS,
    MAX_SSBOS,
};
use crate::{compile_batch, compile_shader, CompileOutcome, OwnedCompileInput};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvCoopLayout {
    pub supported: u8,
    pub use_fp16_accum: u8,
    pub _pad: [u8; 2],
    pub tile_m: u32,
    pub tile_n: u32,
    pub tile_k: u32,
    pub subgroup_size: u32,
    pub a_word_map: [u32; 4],
    pub b_word_map: [u32; 2],
    pub c_word_map: [u32; 2],
    pub d_word_map: [u32; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvProfile {
    pub supported_spirv: u32,
    pub max_subgroup_size: u32,
    pub supported_subgroup_stages: u32,
    pub max_user_clip_distances: u32,
    pub min_ssbo_alignment: u64,
    pub unified_descriptor_binding: u8,
    pub support_descriptor_aliasing: u8,
    pub support_int8: u8,
    pub support_uniform_and_storage_buffer_8bit: u8,
    pub support_storage_buffer_8bit: u8,
    pub support_int16: u8,
    pub support_uniform_and_storage_buffer_16bit: u8,
    pub support_storage_buffer_16bit: u8,
    pub support_int64: u8,
    pub support_float16: u8,
    pub support_float64: u8,
    pub support_float_controls: u8,
    pub support_explicit_workgroup_layout: u8,
    pub support_workgroup_layout_8bit_access: u8,
    pub support_workgroup_layout_16bit_access: u8,
    pub support_vote: u8,
    pub support_shuffle_relative: u8,
    pub support_quad_shuffles: u8,
    pub support_demote_to_helper_invocation: u8,
    pub support_int64_atomics: u8,
    pub support_shared_int64_atomics: u8,
    pub support_typeless_image_loads: u8,
    pub warp_size_potentially_larger_than_guest: u8,
    pub lower_left_origin_mode: u8,
    pub has_broken_spirv_clamp: u8,
    pub has_broken_fp16_float_controls: u8,
    pub has_broken_fp32_denorm_flush: u8,
    pub _pad: [u8; 5],
    pub coop_layout: EglSpirvCoopLayout,
    pub _pad_end: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvBindings {
    pub unified: u32,
    pub uniform_buffer: u32,
    pub storage_buffer: u32,
    pub texture: u32,
    pub image: u32,
    pub texture_scaling_index: u32,
    pub image_scaling_index: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvCbufEntry {
    pub index: u32,
    pub offset: u32,
    pub value: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvTextureTypeEntry {
    pub handle: u32,
    pub texture_type: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvTextureFormatEntry {
    pub handle: u32,
    pub pixel_format: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvStoragePtrEntry {
    pub key: u64,
    pub is_mapped: u8,
    pub _pad: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvCbufReplaceEntry {
    pub bank: u32,
    pub offset: u32,
    pub replace_constant: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EglSpirvCompileInput {
    pub stage: u32,
    pub texture_bound: u32,
    pub program_key: u64,
    pub program_header: [u64; 10],
    pub code: *const EglAmpereInstruction,
    pub code_count: usize,
    pub workgroup_size: [u32; 3],
    pub local_memory_size: u32,
    pub shared_memory_size: u32,
    pub is_task_shader: u8,
    pub is_mesh_shader: u8,
    pub native_tensor: u8,
    pub native_compact_tensor: u8,
    pub has_hle_engine_state: u8,
    pub mesh_as_compute: u8,
    pub _pad0: [u8; 2],
    pub viewport_transform_state: u32,
    pub mesh_max_vertices: u32,
    pub mesh_max_primitives: u32,
    pub mesh_topology: u32,
    pub mesh_payload_words: u32,
    pub _pad1: u32,
    pub cbufs: *const EglSpirvCbufEntry,
    pub cbuf_count: usize,
    pub texture_types: *const EglSpirvTextureTypeEntry,
    pub texture_type_count: usize,
    pub texture_formats: *const EglSpirvTextureFormatEntry,
    pub texture_format_count: usize,
    pub storage_ptrs: *const EglSpirvStoragePtrEntry,
    pub storage_ptr_count: usize,
    pub replacements: *const EglSpirvCbufReplaceEntry,
    pub replacement_count: usize,
    pub profile: EglSpirvProfile,
    pub bindings: EglSpirvBindings,
    pub _pad2: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvCbufDescriptor {
    pub index: u32,
    pub count: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvSsboDescriptor {
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub count: u32,
    pub indirect_offset: u32,
    pub is_written: u8,
    pub _pad: [u8; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvTextureBufferDescriptor {
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub shift_left: u32,
    pub secondary_cbuf_index: u32,
    pub secondary_cbuf_offset: u32,
    pub secondary_shift_left: u32,
    pub count: u32,
    pub size_shift: u32,
    pub handle_program: u32,
    pub has_secondary: u8,
    pub _pad: [u8; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvImageBufferDescriptor {
    pub format: u32,
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub count: u32,
    pub size_shift: u32,
    pub handle_program: u32,
    pub is_written: u8,
    pub is_read: u8,
    pub is_integer: u8,
    pub _pad: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvTextureDescriptor {
    pub type_: u32,
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub shift_left: u32,
    pub secondary_cbuf_index: u32,
    pub secondary_cbuf_offset: u32,
    pub secondary_shift_left: u32,
    pub count: u32,
    pub size_shift: u32,
    pub handle_program: u32,
    pub is_depth: u8,
    pub is_multisample: u8,
    pub is_integer: u8,
    pub has_secondary: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct EglSpirvImageDescriptor {
    pub type_: u32,
    pub format: u32,
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub count: u32,
    pub size_shift: u32,
    pub handle_program: u32,
    pub is_written: u8,
    pub is_read: u8,
    pub is_integer: u8,
    pub _pad: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EglShaderInfoSummary {
    pub constant_buffer_mask: u32,
    pub constant_buffer_used_sizes: [u32; MAX_CBUFS],
    pub used_constant_buffer_types: u32,
    pub used_storage_buffer_types: u32,
    pub used_indirect_cbuf_types: u32,
    pub nvn_buffer_base: u32,
    pub nvn_buffer_used_mask: u32,
    pub used_clip_distances: u32,
    pub stores_frag_color_mask: u32,
    pub frag_color_components: [u8; 8],
    pub uses_workgroup_id: u8,
    pub uses_local_invocation_id: u8,
    pub uses_invocation_id: u8,
    pub uses_invocation_info: u8,
    pub uses_sample_id: u8,
    pub uses_is_helper_invocation: u8,
    pub uses_subgroup_invocation_id: u8,
    pub uses_subgroup_shuffles: u8,
    pub uses_quad_shuffles: u8,
    pub uses_subgroup_vote: u8,
    pub uses_subgroup_mask: u8,
    pub uses_fswzadd: u8,
    pub uses_fp16: u8,
    pub uses_fp64: u8,
    pub uses_fp16_denorms_flush: u8,
    pub uses_fp16_denorms_preserve: u8,
    pub uses_fp32_denorms_flush: u8,
    pub uses_fp32_denorms_preserve: u8,
    pub uses_int8: u8,
    pub uses_int16: u8,
    pub uses_int64: u8,
    pub uses_local_memory: u8,
    pub uses_global_memory: u8,
    pub stores_global_memory: u8,
    pub requires_coherent_global_memory: u8,
    pub requires_coherent_image_memory: u8,
    pub uses_cooperative_matrix: u8,
    pub requires_full_subgroups: u8,
    pub native_global_bounds: u8,
    pub uses_cbuf_indirect: u8,
    pub uses_rescaling_uniform: u8,
    pub uses_render_area: u8,
    pub stores_frag_depth: u8,
    pub stores_sample_mask: u8,
    pub uses_demote_to_helper_invocation: u8,
    pub uses_derivatives: u8,
    pub uses_typeless_image_reads: u8,
    pub uses_typeless_image_writes: u8,
    pub uses_shared_increment: u8,
    pub uses_shared_decrement: u8,
    pub uses_global_increment: u8,
    pub uses_global_decrement: u8,
    pub uses_atomic_f32_add: u8,
    pub uses_atomic_s32_min: u8,
    pub uses_atomic_s32_max: u8,
    pub uses_int64_bit_atomics: u8,
    pub is_task: u8,
    pub is_mesh: u8,
    pub uses_push_constants: u8,
    pub uses_image_1d: u8,
    pub uses_sampled_1d: u8,
    pub uses_image_buffers: u8,
    pub uses_atomic_image_u32: u8,
    pub removed_conditional_barrier: u8,
    pub uses_shadow_lod: u8,
    pub _pad_flags: [u8; 1],
    pub constant_buffer_descriptors: [EglSpirvCbufDescriptor; MAX_CBUFS],
    pub constant_buffer_descriptor_count: usize,
    pub storage_buffer_descriptors: [EglSpirvSsboDescriptor; MAX_SSBOS],
    pub storage_buffer_descriptor_count: usize,
    pub texture_buffer_descriptors: *mut EglSpirvTextureBufferDescriptor,
    pub texture_buffer_descriptor_count: usize,
    pub image_buffer_descriptors: *mut EglSpirvImageBufferDescriptor,
    pub image_buffer_descriptor_count: usize,
    pub texture_descriptors: *mut EglSpirvTextureDescriptor,
    pub texture_descriptor_count: usize,
    pub image_descriptors: *mut EglSpirvImageDescriptor,
    pub image_descriptor_count: usize,
}

impl Default for EglShaderInfoSummary {
    fn default() -> Self {
        Self {
            constant_buffer_mask: 0,
            constant_buffer_used_sizes: [0; MAX_CBUFS],
            used_constant_buffer_types: 0,
            used_storage_buffer_types: 0,
            used_indirect_cbuf_types: 0,
            nvn_buffer_base: 0,
            nvn_buffer_used_mask: 0,
            used_clip_distances: 0,
            stores_frag_color_mask: 0,
            frag_color_components: [0; 8],
            uses_workgroup_id: 0,
            uses_local_invocation_id: 0,
            uses_invocation_id: 0,
            uses_invocation_info: 0,
            uses_sample_id: 0,
            uses_is_helper_invocation: 0,
            uses_subgroup_invocation_id: 0,
            uses_subgroup_shuffles: 0,
            uses_quad_shuffles: 0,
            uses_subgroup_vote: 0,
            uses_subgroup_mask: 0,
            uses_fswzadd: 0,
            uses_fp16: 0,
            uses_fp64: 0,
            uses_fp16_denorms_flush: 0,
            uses_fp16_denorms_preserve: 0,
            uses_fp32_denorms_flush: 0,
            uses_fp32_denorms_preserve: 0,
            uses_int8: 0,
            uses_int16: 0,
            uses_int64: 0,
            uses_local_memory: 0,
            uses_global_memory: 0,
            stores_global_memory: 0,
            requires_coherent_global_memory: 0,
            requires_coherent_image_memory: 0,
            uses_cooperative_matrix: 0,
            requires_full_subgroups: 0,
            native_global_bounds: 0,
            uses_cbuf_indirect: 0,
            uses_rescaling_uniform: 0,
            uses_render_area: 0,
            stores_frag_depth: 0,
            stores_sample_mask: 0,
            uses_demote_to_helper_invocation: 0,
            uses_derivatives: 0,
            uses_typeless_image_reads: 0,
            uses_typeless_image_writes: 0,
            uses_shared_increment: 0,
            uses_shared_decrement: 0,
            uses_global_increment: 0,
            uses_global_decrement: 0,
            uses_atomic_f32_add: 0,
            uses_atomic_s32_min: 0,
            uses_atomic_s32_max: 0,
            uses_int64_bit_atomics: 0,
            is_task: 0,
            is_mesh: 0,
            uses_push_constants: 0,
            uses_image_1d: 0,
            uses_sampled_1d: 0,
            uses_image_buffers: 0,
            uses_atomic_image_u32: 0,
            removed_conditional_barrier: 0,
            uses_shadow_lod: 0,
            _pad_flags: [0],
            constant_buffer_descriptors: [EglSpirvCbufDescriptor::default(); MAX_CBUFS],
            constant_buffer_descriptor_count: 0,
            storage_buffer_descriptors: [EglSpirvSsboDescriptor::default(); MAX_SSBOS],
            storage_buffer_descriptor_count: 0,
            texture_buffer_descriptors: ptr::null_mut(),
            texture_buffer_descriptor_count: 0,
            image_buffer_descriptors: ptr::null_mut(),
            image_buffer_descriptor_count: 0,
            texture_descriptors: ptr::null_mut(),
            texture_descriptor_count: 0,
            image_descriptors: ptr::null_mut(),
            image_descriptor_count: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EglSpirvCompileOutput {
    pub spirv_words: *mut u32,
    pub word_count: usize,
    pub fallback_to_maxwell: u8,
    pub _pad: [u8; 3],
    pub fallback_offset: u32,
    pub fallback_opcode: u32,
    pub error_offset: u32,
    pub error: *mut c_char,
    pub error_len: usize,
    pub updated_bindings: EglSpirvBindings,
    pub _pad_bindings: u32,
    pub info: EglShaderInfoSummary,
}

impl Default for EglSpirvCompileOutput {
    fn default() -> Self {
        Self {
            spirv_words: ptr::null_mut(),
            word_count: 0,
            fallback_to_maxwell: 0,
            _pad: [0; 3],
            fallback_offset: 0,
            fallback_opcode: 0,
            error_offset: 0,
            error: ptr::null_mut(),
            error_len: 0,
            updated_bindings: EglSpirvBindings::default(),
            _pad_bindings: 0,
            info: EglShaderInfoSummary::default(),
        }
    }
}

const _: () = {
    assert!(std::mem::size_of::<EglSpirvCoopLayout>() == 60);
    assert!(std::mem::size_of::<EglSpirvProfile>() == 120);
    assert!(std::mem::size_of::<EglSpirvBindings>() == 28);
};

#[inline]
unsafe fn checked_slice<'a, T>(ptr_val: *const T, count: usize) -> Option<&'a [T]> {
    if count == 0 {
        return Some(&[]);
    }
    if ptr_val.is_null() || (ptr_val as usize) % std::mem::align_of::<T>() != 0 {
        return None;
    }
    let bytes = std::mem::size_of::<T>().checked_mul(count)?;
    if bytes > isize::MAX as usize {
        return None;
    }
    Some(unsafe { std::slice::from_raw_parts(ptr_val, count) })
}

#[inline]
fn is_valid_mut_ptr<T>(ptr_val: *mut T, count: usize) -> bool {
    if count == 0 {
        return true;
    }
    if ptr_val.is_null() || (ptr_val as usize) % std::mem::align_of::<T>() != 0 {
        return false;
    }
    std::mem::size_of::<T>()
        .checked_mul(count)
        .is_some_and(|b| b <= isize::MAX as usize)
}

#[inline]
unsafe fn read_aligned<T: Copy>(ptr_val: *const T) -> Option<T> {
    if ptr_val.is_null() || (ptr_val as usize) % std::mem::align_of::<T>() != 0 {
        return None;
    }
    Some(unsafe { ptr::read(ptr_val) })
}

fn convert_profile(p: &EglSpirvProfile) -> Profile {
    Profile {
        supported_spirv: p.supported_spirv,
        max_subgroup_size: p.max_subgroup_size,
        supported_subgroup_stages: p.supported_subgroup_stages,
        max_user_clip_distances: p.max_user_clip_distances,
        min_ssbo_alignment: p.min_ssbo_alignment,
        unified_descriptor_binding: p.unified_descriptor_binding != 0,
        support_descriptor_aliasing: p.support_descriptor_aliasing != 0,
        support_int8: p.support_int8 != 0,
        support_uniform_and_storage_buffer_8bit: p.support_uniform_and_storage_buffer_8bit != 0,
        support_storage_buffer_8bit: p.support_storage_buffer_8bit != 0,
        support_int16: p.support_int16 != 0,
        support_uniform_and_storage_buffer_16bit: p.support_uniform_and_storage_buffer_16bit != 0,
        support_storage_buffer_16bit: p.support_storage_buffer_16bit != 0,
        support_int64: p.support_int64 != 0,
        support_float16: p.support_float16 != 0,
        support_float64: p.support_float64 != 0,
        support_float_controls: p.support_float_controls != 0,
        support_explicit_workgroup_layout: p.support_explicit_workgroup_layout != 0,
        support_workgroup_layout_8bit_access: p.support_workgroup_layout_8bit_access != 0,
        support_workgroup_layout_16bit_access: p.support_workgroup_layout_16bit_access != 0,
        support_vote: p.support_vote != 0,
        support_shuffle_relative: p.support_shuffle_relative != 0,
        support_quad_shuffles: p.support_quad_shuffles != 0,
        support_demote_to_helper_invocation: p.support_demote_to_helper_invocation != 0,
        support_int64_atomics: p.support_int64_atomics != 0,
        support_shared_int64_atomics: p.support_shared_int64_atomics != 0,
        support_typeless_image_loads: p.support_typeless_image_loads != 0,
        warp_size_potentially_larger_than_guest: p.warp_size_potentially_larger_than_guest != 0,
        lower_left_origin_mode: p.lower_left_origin_mode != 0,
        has_broken_spirv_clamp: p.has_broken_spirv_clamp != 0,
        has_broken_fp16_float_controls: p.has_broken_fp16_float_controls != 0,
        has_broken_fp32_denorm_flush: p.has_broken_fp32_denorm_flush != 0,
        coop_layout: CoopConfig {
            supported: p.coop_layout.supported != 0,
            use_fp16_accum: p.coop_layout.use_fp16_accum != 0,
            tile_m: p.coop_layout.tile_m,
            tile_n: p.coop_layout.tile_n,
            tile_k: p.coop_layout.tile_k,
            subgroup_size: p.coop_layout.subgroup_size,
            a_word_map: p.coop_layout.a_word_map,
            b_word_map: p.coop_layout.b_word_map,
            c_word_map: p.coop_layout.c_word_map,
            d_word_map: p.coop_layout.d_word_map,
        },
    }
}

fn convert_bindings(b: &EglSpirvBindings) -> Bindings {
    Bindings {
        unified: b.unified,
        uniform_buffer: b.uniform_buffer,
        storage_buffer: b.storage_buffer,
        texture: b.texture,
        image: b.image,
        texture_scaling_index: b.texture_scaling_index,
        image_scaling_index: b.image_scaling_index,
    }
}

unsafe fn parse_owned_input(input: &EglSpirvCompileInput) -> Option<OwnedCompileInput> {
    let stage = Stage::from_u32(input.stage)?;
    let code_raw = unsafe { checked_slice(input.code, input.code_count)? };
    let code = code_raw
        .iter()
        .map(|insn| Instruction { words: insn.words })
        .collect();

    let mut snapshot = SnapshotEnv::default();
    for entry in unsafe { checked_slice(input.cbufs, input.cbuf_count)? } {
        snapshot.cbufs.insert((entry.index, entry.offset), entry.value);
    }
    for entry in unsafe { checked_slice(input.texture_types, input.texture_type_count)? } {
        snapshot.texture_types.insert(entry.handle, entry.texture_type);
    }
    for entry in unsafe { checked_slice(input.texture_formats, input.texture_format_count)? } {
        snapshot.texture_formats.insert(entry.handle, entry.pixel_format);
    }
    for entry in unsafe { checked_slice(input.storage_ptrs, input.storage_ptr_count)? } {
        snapshot.storage_ptrs.insert(entry.key, entry.is_mapped != 0);
    }
    for entry in unsafe { checked_slice(input.replacements, input.replacement_count)? } {
        snapshot
            .replacements
            .insert((entry.bank, entry.offset), entry.replace_constant);
    }

    Some(OwnedCompileInput {
        stage,
        program_key: input.program_key,
        code,
        workgroup_size: input.workgroup_size,
        local_memory_size: input.local_memory_size,
        shared_memory_size: input.shared_memory_size,
        profile: convert_profile(&input.profile),
        bindings: convert_bindings(&input.bindings),
        snapshot,
    })
}

fn box_slice_into_raw<T>(vec: Vec<T>) -> (*mut T, usize) {
    if vec.is_empty() {
        return (ptr::null_mut(), 0);
    }
    let len = vec.len();
    let boxed: Box<[T]> = vec.into_boxed_slice();
    (Box::into_raw(boxed) as *mut T, len)
}

unsafe fn free_raw_slice<T>(ptr_val: *mut T, len: usize) {
    if !ptr_val.is_null() && len > 0 && (ptr_val as usize) % std::mem::align_of::<T>() == 0 {
        let slice_ptr = ptr::slice_from_raw_parts_mut(ptr_val, len);
        drop(unsafe { Box::from_raw(slice_ptr) });
    }
}

fn write_outcome_to_c(outcome: CompileOutcome, out: &mut EglSpirvCompileOutput) {
    *out = EglSpirvCompileOutput::default();
    out.updated_bindings = EglSpirvBindings {
        unified: outcome.updated_bindings.unified,
        uniform_buffer: outcome.updated_bindings.uniform_buffer,
        storage_buffer: outcome.updated_bindings.storage_buffer,
        texture: outcome.updated_bindings.texture,
        image: outcome.updated_bindings.image,
        texture_scaling_index: outcome.updated_bindings.texture_scaling_index,
        image_scaling_index: outcome.updated_bindings.image_scaling_index,
    };

    if let Some(fb) = outcome.fallback {
        out.fallback_to_maxwell = 1;
        out.fallback_offset = fb.offset;
        out.fallback_opcode = fb.opcode;
        return;
    }

    let (words_ptr, words_len) = box_slice_into_raw(outcome.spirv_words);
    out.spirv_words = words_ptr;
    out.word_count = words_len;
    out.info = info_to_c(&outcome.info);
}

fn info_to_c(info: &ShaderInfo) -> EglShaderInfoSummary {
    let mut out = EglShaderInfoSummary::default();
    out.constant_buffer_mask = info.constant_buffer_mask;
    out.constant_buffer_used_sizes = info.constant_buffer_used_sizes;
    out.used_constant_buffer_types = info.used_constant_buffer_types;
    out.used_storage_buffer_types = info.used_storage_buffer_types;
    out.used_indirect_cbuf_types = info.used_indirect_cbuf_types;
    out.nvn_buffer_base = info.nvn_buffer_base;
    out.nvn_buffer_used_mask = info.nvn_buffer_used_mask;
    out.used_clip_distances = info.used_clip_distances;
    out.stores_frag_color_mask = info.stores_frag_color_mask;
    out.frag_color_components = info.frag_color_components;

    out.uses_workgroup_id = u8::from(info.uses_workgroup_id);
    out.uses_local_invocation_id = u8::from(info.uses_local_invocation_id);
    out.uses_invocation_id = u8::from(info.uses_invocation_id);
    out.uses_invocation_info = u8::from(info.uses_invocation_info);
    out.uses_sample_id = u8::from(info.uses_sample_id);
    out.uses_is_helper_invocation = u8::from(info.uses_is_helper_invocation);
    out.uses_subgroup_invocation_id = u8::from(info.uses_subgroup_invocation_id);
    out.uses_subgroup_shuffles = u8::from(info.uses_subgroup_shuffles);
    out.uses_quad_shuffles = u8::from(info.uses_quad_shuffles);
    out.uses_subgroup_vote = u8::from(info.uses_subgroup_vote);
    out.uses_subgroup_mask = u8::from(info.uses_subgroup_mask);
    out.uses_fswzadd = u8::from(info.uses_fswzadd);
    out.uses_fp16 = u8::from(info.uses_fp16);
    out.uses_fp64 = u8::from(info.uses_fp64);
    out.uses_fp16_denorms_flush = u8::from(info.uses_fp16_denorms_flush);
    out.uses_fp16_denorms_preserve = u8::from(info.uses_fp16_denorms_preserve);
    out.uses_fp32_denorms_flush = u8::from(info.uses_fp32_denorms_flush);
    out.uses_fp32_denorms_preserve = u8::from(info.uses_fp32_denorms_preserve);
    out.uses_int8 = u8::from(info.uses_int8);
    out.uses_int16 = u8::from(info.uses_int16);
    out.uses_int64 = u8::from(info.uses_int64);
    out.uses_local_memory = u8::from(info.uses_local_memory);
    out.uses_global_memory = u8::from(info.uses_global_memory);
    out.stores_global_memory = u8::from(info.stores_global_memory);
    out.requires_coherent_global_memory = u8::from(info.requires_coherent_global_memory);
    out.requires_coherent_image_memory = u8::from(info.requires_coherent_image_memory);
    out.uses_cooperative_matrix = u8::from(info.uses_cooperative_matrix);
    out.requires_full_subgroups = u8::from(info.requires_full_subgroups);
    out.native_global_bounds = u8::from(info.native_global_bounds);
    out.uses_cbuf_indirect = u8::from(info.uses_cbuf_indirect);
    out.uses_rescaling_uniform = u8::from(info.uses_rescaling_uniform);
    out.uses_render_area = u8::from(info.uses_render_area);
    out.stores_frag_depth = u8::from(info.stores_frag_depth);
    out.stores_sample_mask = u8::from(info.stores_sample_mask);
    out.uses_demote_to_helper_invocation = u8::from(info.uses_demote_to_helper_invocation);
    out.uses_derivatives = u8::from(info.uses_derivatives);
    out.uses_typeless_image_reads = u8::from(info.uses_typeless_image_reads);
    out.uses_typeless_image_writes = u8::from(info.uses_typeless_image_writes);
    out.uses_shared_increment = u8::from(info.uses_shared_increment);
    out.uses_shared_decrement = u8::from(info.uses_shared_decrement);
    out.uses_global_increment = u8::from(info.uses_global_increment);
    out.uses_global_decrement = u8::from(info.uses_global_decrement);
    out.uses_atomic_f32_add = u8::from(info.uses_atomic_f32_add);
    out.uses_atomic_s32_min = u8::from(info.uses_atomic_s32_min);
    out.uses_atomic_s32_max = u8::from(info.uses_atomic_s32_max);
    out.uses_int64_bit_atomics = u8::from(info.uses_int64_bit_atomics);
    out.is_task = u8::from(info.is_task);
    out.is_mesh = u8::from(info.is_mesh);
    out.uses_push_constants = u8::from(info.uses_push_constants);
    out.uses_image_1d = u8::from(info.uses_image_1d);
    out.uses_sampled_1d = u8::from(info.uses_sampled_1d);
    out.uses_image_buffers = u8::from(info.uses_image_buffers);
    out.uses_atomic_image_u32 = u8::from(info.uses_atomic_image_u32);
    out.removed_conditional_barrier = u8::from(info.removed_conditional_barrier);
    out.uses_shadow_lod = u8::from(info.uses_shadow_lod);

    let cbuf_n = info.constant_buffer_descriptors.len().min(MAX_CBUFS);
    for (i, d) in info.constant_buffer_descriptors.iter().take(cbuf_n).enumerate() {
        out.constant_buffer_descriptors[i] = convert_cbuf_desc(d);
    }
    out.constant_buffer_descriptor_count = cbuf_n;

    let ssbo_n = info.storage_buffer_descriptors.len().min(MAX_SSBOS);
    for (i, d) in info.storage_buffer_descriptors.iter().take(ssbo_n).enumerate() {
        out.storage_buffer_descriptors[i] = convert_ssbo_desc(d);
    }
    out.storage_buffer_descriptor_count = ssbo_n;

    let (tb_ptr, tb_len) = box_slice_into_raw(
        info.texture_buffer_descriptors
            .iter()
            .map(convert_tbuf_desc)
            .collect(),
    );
    out.texture_buffer_descriptors = tb_ptr;
    out.texture_buffer_descriptor_count = tb_len;

    let (ib_ptr, ib_len) = box_slice_into_raw(
        info.image_buffer_descriptors
            .iter()
            .map(convert_ibuf_desc)
            .collect(),
    );
    out.image_buffer_descriptors = ib_ptr;
    out.image_buffer_descriptor_count = ib_len;

    let (t_ptr, t_len) = box_slice_into_raw(
        info.texture_descriptors
            .iter()
            .map(convert_tex_desc)
            .collect(),
    );
    out.texture_descriptors = t_ptr;
    out.texture_descriptor_count = t_len;

    let (i_ptr, i_len) = box_slice_into_raw(
        info.image_descriptors
            .iter()
            .map(convert_img_desc)
            .collect(),
    );
    out.image_descriptors = i_ptr;
    out.image_descriptor_count = i_len;
    out
}

fn convert_cbuf_desc(d: &CbufDescriptor) -> EglSpirvCbufDescriptor {
    EglSpirvCbufDescriptor {
        index: d.index,
        count: d.count,
    }
}

fn convert_ssbo_desc(d: &SsboDescriptor) -> EglSpirvSsboDescriptor {
    EglSpirvSsboDescriptor {
        cbuf_index: d.cbuf_index,
        cbuf_offset: d.cbuf_offset,
        count: d.count,
        indirect_offset: d.indirect_offset,
        is_written: u8::from(d.is_written),
        _pad: [0; 3],
    }
}

fn convert_tbuf_desc(d: &TextureBufferDescriptor) -> EglSpirvTextureBufferDescriptor {
    EglSpirvTextureBufferDescriptor {
        cbuf_index: d.cbuf_index,
        cbuf_offset: d.cbuf_offset,
        shift_left: d.shift_left,
        secondary_cbuf_index: d.secondary_cbuf_index,
        secondary_cbuf_offset: d.secondary_cbuf_offset,
        secondary_shift_left: d.secondary_shift_left,
        count: d.count,
        size_shift: d.size_shift,
        handle_program: d.handle_program,
        has_secondary: u8::from(d.has_secondary),
        _pad: [0; 3],
    }
}

fn convert_ibuf_desc(d: &ImageBufferDescriptor) -> EglSpirvImageBufferDescriptor {
    EglSpirvImageBufferDescriptor {
        format: d.format,
        cbuf_index: d.cbuf_index,
        cbuf_offset: d.cbuf_offset,
        count: d.count,
        size_shift: d.size_shift,
        handle_program: d.handle_program,
        is_written: u8::from(d.is_written),
        is_read: u8::from(d.is_read),
        is_integer: u8::from(d.is_integer),
        _pad: 0,
    }
}

fn convert_tex_desc(d: &TextureDescriptor) -> EglSpirvTextureDescriptor {
    EglSpirvTextureDescriptor {
        type_: d.type_,
        cbuf_index: d.cbuf_index,
        cbuf_offset: d.cbuf_offset,
        shift_left: d.shift_left,
        secondary_cbuf_index: d.secondary_cbuf_index,
        secondary_cbuf_offset: d.secondary_cbuf_offset,
        secondary_shift_left: d.secondary_shift_left,
        count: d.count,
        size_shift: d.size_shift,
        handle_program: d.handle_program,
        is_depth: u8::from(d.is_depth),
        is_multisample: u8::from(d.is_multisample),
        is_integer: u8::from(d.is_integer),
        has_secondary: u8::from(d.has_secondary),
    }
}

fn convert_img_desc(d: &ImageDescriptor) -> EglSpirvImageDescriptor {
    EglSpirvImageDescriptor {
        type_: d.type_,
        format: d.format,
        cbuf_index: d.cbuf_index,
        cbuf_offset: d.cbuf_offset,
        count: d.count,
        size_shift: d.size_shift,
        handle_program: d.handle_program,
        is_written: u8::from(d.is_written),
        is_read: u8::from(d.is_read),
        is_integer: u8::from(d.is_integer),
        _pad: 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn egl_ampere_spirv_compile(
    input: *const EglSpirvCompileInput,
    out_output: *mut EglSpirvCompileOutput,
) -> i32 {
    let res = catch_unwind(AssertUnwindSafe(|| unsafe {
        if !is_valid_mut_ptr(out_output, 1) || out_output.is_null() {
            return -1;
        }
        let Some(c_in) = read_aligned(input) else {
            return -1;
        };
        let Some(owned) = parse_owned_input(&c_in) else {
            return -1;
        };
        let outcome = compile_shader(&owned.as_lower_input());
        let mut c_out = EglSpirvCompileOutput::default();
        write_outcome_to_c(outcome, &mut c_out);
        ptr::write(out_output, c_out);
        0
    }));
    res.unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_ampere_spirv_compile_batch(
    inputs: *const EglSpirvCompileInput,
    count: usize,
    outputs: *mut EglSpirvCompileOutput,
) -> i32 {
    let res = catch_unwind(AssertUnwindSafe(|| unsafe {
        if count == 0 {
            return 0;
        }
        let Some(c_inputs) = checked_slice(inputs, count) else {
            return -1;
        };
        if !is_valid_mut_ptr(outputs, count) || outputs.is_null() {
            return -1;
        }
        let mut owned_inputs = Vec::with_capacity(count);
        for c_in in c_inputs {
            let Some(owned) = parse_owned_input(c_in) else {
                return -1;
            };
            owned_inputs.push(owned);
        }
        let outcomes = compile_batch(&owned_inputs);
        let c_outs = std::slice::from_raw_parts_mut(outputs, count);
        for (outcome, slot) in outcomes.into_iter().zip(c_outs.iter_mut()) {
            let mut c_out = EglSpirvCompileOutput::default();
            write_outcome_to_c(outcome, &mut c_out);
            *slot = c_out;
        }
        0
    }));
    res.unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_ampere_spirv_free(output: *mut EglSpirvCompileOutput) {
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        if output.is_null() || (output as usize) % std::mem::align_of::<EglSpirvCompileOutput>() != 0
        {
            return;
        }
        let out = &mut *output;
        free_raw_slice(out.spirv_words, out.word_count);
        if !out.error.is_null() && out.error_len > 0 {
            free_raw_slice(out.error as *mut u8, out.error_len);
        }
        free_raw_slice(
            out.info.texture_buffer_descriptors,
            out.info.texture_buffer_descriptor_count,
        );
        free_raw_slice(
            out.info.image_buffer_descriptors,
            out.info.image_buffer_descriptor_count,
        );
        free_raw_slice(
            out.info.texture_descriptors,
            out.info.texture_descriptor_count,
        );
        free_raw_slice(
            out.info.image_descriptors,
            out.info.image_descriptor_count,
        );
        *out = EglSpirvCompileOutput::default();
    }));
}
