// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::collections::BTreeMap;

use crate::coop_matrix::CoopConfig;

pub const SPIRV_1_6_VERSION: u32 = 0x0001_0600;
pub const MAX_CBUFS: usize = 18;
pub const MAX_SSBOS: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Stage {
    VertexB = 0,
    TessControl = 1,
    TessEval = 2,
    Geometry = 3,
    Fragment = 4,
    Compute = 5,
    VertexA = 6,
    Task = 7,
    Mesh = 8,
}

impl Stage {
    #[must_use]
    pub const fn from_u32(val: u32) -> Option<Self> {
        match val {
            0 => Some(Self::VertexB),
            1 => Some(Self::TessControl),
            2 => Some(Self::TessEval),
            3 => Some(Self::Geometry),
            4 => Some(Self::Fragment),
            5 => Some(Self::Compute),
            6 => Some(Self::VertexA),
            7 => Some(Self::Task),
            8 => Some(Self::Mesh),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Profile {
    pub supported_spirv: u32,
    pub max_subgroup_size: u32,
    pub supported_subgroup_stages: u32,
    pub max_user_clip_distances: u32,
    pub min_ssbo_alignment: u64,
    pub unified_descriptor_binding: bool,
    pub support_descriptor_aliasing: bool,
    pub support_int8: bool,
    pub support_uniform_and_storage_buffer_8bit: bool,
    pub support_storage_buffer_8bit: bool,
    pub support_int16: bool,
    pub support_uniform_and_storage_buffer_16bit: bool,
    pub support_storage_buffer_16bit: bool,
    pub support_int64: bool,
    pub support_float16: bool,
    pub support_float64: bool,
    pub support_float_controls: bool,
    pub support_explicit_workgroup_layout: bool,
    pub support_workgroup_layout_8bit_access: bool,
    pub support_workgroup_layout_16bit_access: bool,
    pub support_vote: bool,
    pub support_shuffle_relative: bool,
    pub support_quad_shuffles: bool,
    pub support_demote_to_helper_invocation: bool,
    pub support_int64_atomics: bool,
    pub support_shared_int64_atomics: bool,
    pub support_typeless_image_loads: bool,
    pub warp_size_potentially_larger_than_guest: bool,
    pub lower_left_origin_mode: bool,
    pub has_broken_spirv_clamp: bool,
    pub has_broken_fp16_float_controls: bool,
    pub has_broken_fp32_denorm_flush: bool,
    pub coop_layout: CoopConfig,
}

impl Profile {
    /// Returns a typical Vulkan 1.3+ / SPIR-V 1.6 desktop/Switch-capable test profile.
    #[must_use]
    pub fn spirv_1_6_default() -> Self {
        Self {
            supported_spirv: SPIRV_1_6_VERSION,
            max_subgroup_size: 32,
            supported_subgroup_stages: u32::MAX,
            max_user_clip_distances: 8,
            min_ssbo_alignment: 16,
            support_int8: true,
            support_uniform_and_storage_buffer_8bit: true,
            support_storage_buffer_8bit: true,
            support_int16: true,
            support_uniform_and_storage_buffer_16bit: true,
            support_storage_buffer_16bit: true,
            support_int64: true,
            support_float16: true,
            support_float64: false,
            support_float_controls: true,
            support_explicit_workgroup_layout: true,
            support_workgroup_layout_8bit_access: true,
            support_workgroup_layout_16bit_access: true,
            support_vote: true,
            support_shuffle_relative: true,
            support_quad_shuffles: true,
            support_int64_atomics: true,
            support_shared_int64_atomics: true,
            coop_layout: CoopConfig {
                supported: true,
                ..CoopConfig::default()
            },
            ..Self::default()
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Bindings {
    pub unified: u32,
    pub uniform_buffer: u32,
    pub storage_buffer: u32,
    pub texture: u32,
    pub image: u32,
    pub texture_scaling_index: u32,
    pub image_scaling_index: u32,
}

#[derive(Clone, Debug, Default)]
pub struct SnapshotEnv {
    pub cbufs: BTreeMap<(u32, u32), u32>,
    pub texture_types: BTreeMap<u32, u32>,
    pub texture_formats: BTreeMap<u32, u32>,
    pub storage_ptrs: BTreeMap<u64, bool>,
    pub replacements: BTreeMap<(u32, u32), u32>,
}

impl SnapshotEnv {
    #[inline]
    #[must_use]
    pub fn make_key(cbuf_index: u32, cbuf_offset: u32) -> u64 {
        (u64::from(cbuf_index) << 32) | u64::from(cbuf_offset)
    }

    #[must_use]
    pub fn lookup_cbuf(&self, bank: u32, offset: u32) -> Option<u32> {
        self.replacements
            .get(&(bank, offset))
            .copied()
            .or_else(|| self.cbufs.get(&(bank, offset)).copied())
    }

    #[must_use]
    pub fn lookup_replacement(&self, bank: u32, offset: u32) -> Option<u32> {
        self.replacements.get(&(bank, offset)).copied()
    }

    #[must_use]
    pub fn is_mapped_storage_ptr(&self, bank: u32, offset: u32) -> Option<bool> {
        self.storage_ptrs.get(&Self::make_key(bank, offset)).copied()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct CbufDescriptor {
    pub index: u32,
    pub count: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct SsboDescriptor {
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub count: u32,
    pub indirect_offset: u32,
    pub is_written: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct TextureBufferDescriptor {
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub shift_left: u32,
    pub secondary_cbuf_index: u32,
    pub secondary_cbuf_offset: u32,
    pub secondary_shift_left: u32,
    pub count: u32,
    pub size_shift: u32,
    pub handle_program: u32,
    pub has_secondary: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct ImageBufferDescriptor {
    pub format: u32,
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub count: u32,
    pub size_shift: u32,
    pub handle_program: u32,
    pub is_written: bool,
    pub is_read: bool,
    pub is_integer: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct TextureDescriptor {
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
    pub is_depth: bool,
    pub is_multisample: bool,
    pub is_integer: bool,
    pub has_secondary: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct ImageDescriptor {
    pub type_: u32,
    pub format: u32,
    pub cbuf_index: u32,
    pub cbuf_offset: u32,
    pub count: u32,
    pub size_shift: u32,
    pub handle_program: u32,
    pub is_written: bool,
    pub is_read: bool,
    pub is_integer: bool,
}

/// Complete reflected shader metadata corresponding 1:1 to `Shader::Info` / `EglShaderInfoSummary`.
#[derive(Clone, Debug, Default)]
pub struct ShaderInfo {
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
    pub uses_workgroup_id: bool,
    pub uses_local_invocation_id: bool,
    pub uses_invocation_id: bool,
    pub uses_invocation_info: bool,
    pub uses_sample_id: bool,
    pub uses_is_helper_invocation: bool,
    pub uses_subgroup_invocation_id: bool,
    pub uses_subgroup_shuffles: bool,
    pub uses_quad_shuffles: bool,
    pub uses_subgroup_vote: bool,
    pub uses_subgroup_mask: bool,
    pub uses_fswzadd: bool,
    pub uses_fp16: bool,
    pub uses_fp64: bool,
    pub uses_fp16_denorms_flush: bool,
    pub uses_fp16_denorms_preserve: bool,
    pub uses_fp32_denorms_flush: bool,
    pub uses_fp32_denorms_preserve: bool,
    pub uses_int8: bool,
    pub uses_int16: bool,
    pub uses_int64: bool,
    pub uses_local_memory: bool,
    pub uses_global_memory: bool,
    pub stores_global_memory: bool,
    pub requires_coherent_global_memory: bool,
    pub requires_coherent_image_memory: bool,
    pub uses_cooperative_matrix: bool,
    pub requires_full_subgroups: bool,
    pub native_global_bounds: bool,
    pub uses_cbuf_indirect: bool,
    pub uses_rescaling_uniform: bool,
    pub uses_render_area: bool,
    pub stores_frag_depth: bool,
    pub stores_sample_mask: bool,
    pub uses_demote_to_helper_invocation: bool,
    pub uses_derivatives: bool,
    pub uses_typeless_image_reads: bool,
    pub uses_typeless_image_writes: bool,
    pub uses_shared_increment: bool,
    pub uses_shared_decrement: bool,
    pub uses_global_increment: bool,
    pub uses_global_decrement: bool,
    pub uses_atomic_f32_add: bool,
    pub uses_atomic_s32_min: bool,
    pub uses_atomic_s32_max: bool,
    pub uses_int64_bit_atomics: bool,
    pub is_task: bool,
    pub is_mesh: bool,
    pub uses_push_constants: bool,
    pub uses_image_1d: bool,
    pub uses_sampled_1d: bool,
    pub uses_image_buffers: bool,
    pub uses_atomic_image_u32: bool,
    pub removed_conditional_barrier: bool,
    pub uses_shadow_lod: bool,
    pub constant_buffer_descriptors: Vec<CbufDescriptor>,
    pub storage_buffer_descriptors: Vec<SsboDescriptor>,
    pub texture_buffer_descriptors: Vec<TextureBufferDescriptor>,
    pub image_buffer_descriptors: Vec<ImageBufferDescriptor>,
    pub texture_descriptors: Vec<TextureDescriptor>,
    pub image_descriptors: Vec<ImageDescriptor>,
}

impl ShaderInfo {
    /// Marks a constant buffer `(bank, byte_offset)` read of `byte_size` bytes.
    pub fn record_cbuf_read(&mut self, bank: u32, byte_offset: u32, byte_size: u32) {
        if (bank as usize) >= MAX_CBUFS {
            return;
        }
        let bit = 1u32 << bank;
        if (self.constant_buffer_mask & bit) == 0 {
            self.constant_buffer_mask |= bit;
            self.constant_buffer_descriptors.push(CbufDescriptor {
                index: bank,
                count: 1,
            });
        }
        let end = byte_offset.saturating_add(byte_size);
        let slot = &mut self.constant_buffer_used_sizes[bank as usize];
        if end > *slot {
            *slot = end;
        }
        // Track BitField / Scalar U32 usage (bit 2 in IR::Type = U32 = 4).
        self.used_constant_buffer_types |= 1u32 << 2;
    }

    /// Registers or merges a storage buffer descriptor backed by `(cbuf_index, cbuf_offset)`.
    pub fn record_ssbo(
        &mut self,
        cbuf_index: u32,
        cbuf_offset: u32,
        written: bool,
        type_mask: u32,
    ) -> Option<usize> {
        self.used_storage_buffer_types |= type_mask;
        if let Some((idx, existing)) = self
            .storage_buffer_descriptors
            .iter_mut()
            .enumerate()
            .find(|(_, d)| d.cbuf_index == cbuf_index && d.cbuf_offset == cbuf_offset)
        {
            if written {
                existing.is_written = true;
            }
            return Some(idx);
        }
        if self.storage_buffer_descriptors.len() >= MAX_SSBOS {
            return None;
        }
        let idx = self.storage_buffer_descriptors.len();
        self.storage_buffer_descriptors.push(SsboDescriptor {
            cbuf_index,
            cbuf_offset,
            count: 1,
            indirect_offset: u32::MAX,
            is_written: written,
        });
        Some(idx)
    }

    /// Sorts cbuf descriptors by binding index cleanly for deterministic descriptor set layout
    /// while preserving storage buffer discovery order and updating Vulkan binding counters.
    pub fn finalize_descriptors(&mut self, bindings: &mut Bindings, unified: bool) {
        self.constant_buffer_descriptors.sort_by_key(|d| d.index);
        for d in &self.constant_buffer_descriptors {
            if unified {
                bindings.unified = bindings.unified.saturating_add(d.count);
            } else {
                bindings.uniform_buffer = bindings.uniform_buffer.saturating_add(d.count);
            }
        }
        for d in &self.storage_buffer_descriptors {
            if unified {
                bindings.unified = bindings.unified.saturating_add(d.count);
            } else {
                bindings.storage_buffer = bindings.storage_buffer.saturating_add(d.count);
            }
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FallbackReason {
    pub offset: u32,
    pub opcode: u32,
    pub reason: &'static str,
}
