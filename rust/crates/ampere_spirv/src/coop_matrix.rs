// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

/// SPIR-V KHR_cooperative_matrix constants (`spirv.hpp`).
pub const CAPABILITY_COOPERATIVE_MATRIX_KHR: u32 = 6022;
pub const OP_TYPE_COOPERATIVE_MATRIX_KHR: u16 = 4456;
pub const OP_COOPERATIVE_MATRIX_MUL_ADD_KHR: u16 = 4459;

pub const COOP_USE_MATRIX_A: u32 = 0;
pub const COOP_USE_MATRIX_B: u32 = 1;
pub const COOP_USE_MATRIX_ACCUMULATOR: u32 = 2;
pub const SCOPE_SUBGROUP: u32 = 3;

/// Validated, normalized cooperative matrix layout matching `EglSpirvCoopLayout`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CoopConfig {
    pub supported: bool,
    pub use_fp16_accum: bool,
    pub tile_m: u32,
    pub tile_n: u32,
    pub tile_k: u32,
    pub subgroup_size: u32,
    pub a_word_map: [u32; 4],
    pub b_word_map: [u32; 2],
    pub c_word_map: [u32; 2],
    pub d_word_map: [u32; 2],
}

impl Default for CoopConfig {
    fn default() -> Self {
        Self {
            supported: false,
            use_fp16_accum: true,
            tile_m: 16,
            tile_n: 8,
            tile_k: 16,
            subgroup_size: 32,
            a_word_map: [0, 2, 1, 3],
            b_word_map: [0, 1],
            c_word_map: [0, 1],
            d_word_map: [0, 1],
        }
    }
}

impl CoopConfig {
    /// Normalizes zero-initialized fields to canonical `16x8x16` defaults while preserving
    /// any caller-supplied parameterization of tile dimensions and word maps.
    #[must_use]
    pub fn normalized(mut self) -> Self {
        if self.tile_m == 0 {
            self.tile_m = 16;
        }
        if self.tile_n == 0 {
            self.tile_n = 8;
        }
        if self.tile_k == 0 {
            self.tile_k = 16;
        }
        if self.subgroup_size == 0 {
            self.subgroup_size = 32;
        }
        if self.a_word_map == [0, 0, 0, 0] {
            self.a_word_map = [0, 2, 1, 3];
        }
        if self.b_word_map == [0, 0] {
            self.b_word_map = [0, 1];
        }
        if self.c_word_map == [0, 0] {
            self.c_word_map = [0, 1];
        }
        if self.d_word_map == [0, 0] {
            self.d_word_map = [0, 1];
        }
        self
    }

    /// Returns the effective K dimension for a given SM86 HMMA depth (`8` or `16`).
    #[inline]
    #[must_use]
    pub const fn effective_tile_k(self, depth: u32) -> u32 {
        if depth == 8 && self.tile_k == 16 {
            8
        } else {
            self.tile_k
        }
    }

    /// Validates hardware support, subgroup size, tile geometry, and permutation maps.
    pub fn validate(&self, depth: u32) -> Result<(), &'static str> {
        if !self.supported {
            return Err("cooperative matrix unsupported by device profile");
        }
        if self.tile_m == 0 || self.tile_n == 0 || self.tile_k == 0 {
            return Err("invalid cooperative matrix tile dimensions");
        }
        if self.subgroup_size != 32 {
            return Err("cooperative matrix requires 32-lane guest subgroup");
        }
        let a_words = (depth / 4).min(4) as usize;
        if !is_valid_map(&self.a_word_map[..a_words], 4) {
            return Err("invalid cooperative matrix a_word_map");
        }
        if !is_valid_map(&self.b_word_map, 2)
            || !is_valid_map(&self.c_word_map, 2)
            || !is_valid_map(&self.d_word_map, 2)
        {
            return Err("invalid cooperative matrix b/c/d word_map");
        }
        Ok(())
    }
}

fn is_valid_map(map: &[u32], limit: usize) -> bool {
    if map.len() > limit {
        return false;
    }
    let mut seen = [false; 4];
    for &idx in map {
        let idx = idx as usize;
        if idx >= limit || seen[idx] {
            return false;
        }
        seen[idx] = true;
    }
    true
}

/// Abstract sink used to construct native `OpTypeCooperativeMatrixKHR` and
/// `OpCooperativeMatrixMulAddKHR` directly in SPIR-V word form without patching hacks.
pub trait CoopSpirvBuilder {
    fn alloc_id(&mut self) -> u32;
    fn const_u32(&mut self, value: u32) -> u32;
    fn type_u32(&mut self) -> u32;
    fn type_f16(&mut self) -> u32;
    fn type_f32(&mut self) -> u32;
    fn type_f16x2(&mut self) -> u32;
    fn type_coop_matrix(&mut self, elem_ty: u32, rows: u32, cols: u32, use_kind: u32) -> u32;
    fn push_fn_op(&mut self, opcode: u16, operands: &[u32]);

    fn op_undef(&mut self, ty: u32) -> u32 {
        let id = self.alloc_id();
        self.push_fn_op(44, &[ty, id]); // OpUndef
        id
    }

    fn op_bitcast(&mut self, ty: u32, val: u32) -> u32 {
        let id = self.alloc_id();
        self.push_fn_op(124, &[ty, id, val]); // OpBitcast
        id
    }

    fn op_composite_construct(&mut self, ty: u32, elems: &[u32]) -> u32 {
        let id = self.alloc_id();
        let mut ops = Vec::with_capacity(2 + elems.len());
        ops.push(ty);
        ops.push(id);
        ops.extend_from_slice(elems);
        self.push_fn_op(80, &ops); // OpCompositeConstruct
        id
    }

    fn op_composite_extract(&mut self, ty: u32, comp: u32, idx: u32) -> u32 {
        let id = self.alloc_id();
        self.push_fn_op(81, &[ty, id, comp, idx]); // OpCompositeExtract
        id
    }

    fn op_composite_insert(&mut self, ty: u32, obj: u32, comp: u32, idx: u32) -> u32 {
        let id = self.alloc_id();
        self.push_fn_op(82, &[ty, id, obj, comp, idx]); // OpCompositeInsert
        id
    }

    fn op_fconvert(&mut self, ty: u32, val: u32) -> u32 {
        let id = self.alloc_id();
        self.push_fn_op(115, &[ty, id, val]); // OpFConvert
        id
    }
}

/// Emits native `OpTypeCooperativeMatrixKHR` types and `OpCooperativeMatrixMulAddKHR` directly
/// from normalized `CoopConfig` parameters (`tile_m`, `tile_n`, `tile_k`, word maps).
pub fn emit_native_coop_hmma<B: CoopSpirvBuilder>(
    b: &mut B,
    config: &CoopConfig,
    depth: u32,
    a_words: &[u32],
    b_words: &[u32],
    c_words: &[u32; 2],
) -> [u32; 2] {
    let f16_ty = b.type_f16();
    let f16x2_ty = b.type_f16x2();
    let u32_ty = b.type_u32();
    let accum_elem_ty = if config.use_fp16_accum {
        f16_ty
    } else {
        b.type_f32()
    };

    let tile_m = config.tile_m;
    let tile_n = config.tile_n;
    let tile_k = config.effective_tile_k(depth);

    let matrix_a_ty = b.type_coop_matrix(f16_ty, tile_m, tile_k, COOP_USE_MATRIX_A);
    let matrix_b_ty = b.type_coop_matrix(f16_ty, tile_k, tile_n, COOP_USE_MATRIX_B);
    let matrix_c_ty = b.type_coop_matrix(accum_elem_ty, tile_m, tile_n, COOP_USE_MATRIX_ACCUMULATOR);

    fn insert_f16_word<B: CoopSpirvBuilder>(
        b: &mut B,
        f16_ty: u32,
        f16x2_ty: u32,
        accum_elem_ty: u32,
        matrix_c_ty: u32,
        use_fp16_accum: bool,
        mat_ty: u32,
        mut mat: u32,
        word_id: u32,
        slot: u32,
    ) -> u32 {
        let halves = b.op_bitcast(f16x2_ty, word_id);
        for half in 0..2u32 {
            let scalar_f16 = b.op_composite_extract(f16_ty, halves, half);
            let elem = if mat_ty == matrix_c_ty && !use_fp16_accum {
                b.op_fconvert(accum_elem_ty, scalar_f16)
            } else {
                scalar_f16
            };
            mat = b.op_composite_insert(mat_ty, elem, mat, slot * 2 + half);
        }
        mat
    }

    let mut mat_a = b.op_undef(matrix_a_ty);
    for (slot, &guest_idx) in config.a_word_map.iter().take(a_words.len()).enumerate() {
        let word_id = a_words[(guest_idx as usize).min(a_words.len() - 1)];
        mat_a = insert_f16_word(
            b,
            f16_ty,
            f16x2_ty,
            accum_elem_ty,
            matrix_c_ty,
            config.use_fp16_accum,
            matrix_a_ty,
            mat_a,
            word_id,
            slot as u32,
        );
    }

    let mut mat_b = b.op_undef(matrix_b_ty);
    for (slot, &guest_idx) in config.b_word_map.iter().take(b_words.len()).enumerate() {
        let word_id = b_words[(guest_idx as usize).min(b_words.len() - 1)];
        mat_b = insert_f16_word(
            b,
            f16_ty,
            f16x2_ty,
            accum_elem_ty,
            matrix_c_ty,
            config.use_fp16_accum,
            matrix_b_ty,
            mat_b,
            word_id,
            slot as u32,
        );
    }

    let mut mat_c = b.op_undef(matrix_c_ty);
    for (slot, &guest_idx) in config.c_word_map.iter().enumerate() {
        let word_id = c_words[(guest_idx as usize).min(1)];
        mat_c = insert_f16_word(
            b,
            f16_ty,
            f16x2_ty,
            accum_elem_ty,
            matrix_c_ty,
            config.use_fp16_accum,
            matrix_c_ty,
            mat_c,
            word_id,
            slot as u32,
        );
    }

    let mat_d = b.alloc_id();
    b.push_fn_op(
        OP_COOPERATIVE_MATRIX_MUL_ADD_KHR,
        &[matrix_c_ty, mat_d, mat_a, mat_b, mat_c],
    );

    let mut out_words = [0u32; 2];
    for (slot, &guest_idx) in config.d_word_map.iter().enumerate() {
        let s = slot as u32;
        let e0 = b.op_composite_extract(accum_elem_ty, mat_d, s * 2);
        let e1 = b.op_composite_extract(accum_elem_ty, mat_d, s * 2 + 1);
        let (h0, h1) = if config.use_fp16_accum {
            (e0, e1)
        } else {
            (b.op_fconvert(f16_ty, e0), b.op_fconvert(f16_ty, e1))
        };
        let packed = b.op_composite_construct(f16x2_ty, &[h0, h1]);
        out_words[(guest_idx as usize).min(1)] = b.op_bitcast(u32_ty, packed);
    }
    out_words
}
