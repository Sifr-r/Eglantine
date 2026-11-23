// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

#[forbid(unsafe_code)]
pub mod coop_matrix;
#[forbid(unsafe_code)]
pub mod decode_sm86;
#[forbid(unsafe_code)]
pub mod reflect;
#[forbid(unsafe_code)]
pub mod spirv_emit;
#[forbid(unsafe_code)]
pub mod ssa_ir;

pub mod ffi;

pub use coop_matrix::CoopConfig;
pub use reflect::{
    Bindings, CbufDescriptor, FallbackReason, ImageBufferDescriptor, ImageDescriptor, Profile,
    ShaderInfo, SnapshotEnv, SsboDescriptor, Stage, TextureBufferDescriptor, TextureDescriptor,
    MAX_CBUFS, MAX_SSBOS, SPIRV_1_6_VERSION,
};
pub use spirv_emit::{emit_spirv_1_6, SPIRV_MAGIC};
pub use ssa_ir::{lower_to_ssa, LowerInput, SsaProgram};

use eglantine_ampere_rs::Instruction;

/// Safe Rust output produced by [`compile_shader`] and [`compile_batch`].
#[derive(Clone, Debug)]
pub struct CompileOutcome {
    pub spirv_words: Vec<u32>,
    pub fallback: Option<FallbackReason>,
    pub updated_bindings: Bindings,
    pub info: ShaderInfo,
}

/// Owned, `Send + Sync` shader compilation request used for safe parallel batch execution.
#[derive(Clone, Debug)]
pub struct OwnedCompileInput {
    pub stage: Stage,
    pub program_key: u64,
    pub code: Vec<Instruction>,
    pub workgroup_size: [u32; 3],
    pub local_memory_size: u32,
    pub shared_memory_size: u32,
    pub profile: Profile,
    pub bindings: Bindings,
    pub snapshot: SnapshotEnv,
}

impl OwnedCompileInput {
    #[forbid(unsafe_code)]
    #[must_use]
    pub fn as_lower_input(&self) -> LowerInput<'_> {
        LowerInput {
            stage: self.stage,
            program_key: self.program_key,
            code: &self.code,
            workgroup_size: self.workgroup_size,
            local_memory_size: self.local_memory_size,
            shared_memory_size: self.shared_memory_size,
            profile: self.profile,
            bindings: self.bindings,
            snapshot: self.snapshot.clone(),
        }
    }
}

/// Compiles one SM86 shader program directly to SPIR-V 1.6 words, or returns a clean
/// `fallback` reason (`fallback_to_maxwell = 1`) with zero errors when a feature is unlowered.
#[forbid(unsafe_code)]
#[must_use]
pub fn compile_shader(input: &LowerInput<'_>) -> CompileOutcome {
    match lower_to_ssa(input) {
        Ok(ssa) => {
            let spirv_words = emit_spirv_1_6(&ssa);
            CompileOutcome {
                spirv_words,
                fallback: None,
                updated_bindings: ssa.bindings,
                info: ssa.info,
            }
        }
        Err(reason) => CompileOutcome {
            spirv_words: Vec::new(),
            fallback: Some(reason),
            updated_bindings: input.bindings,
            info: ShaderInfo::default(),
        },
    }
}

/// Compiles a slice of independent SM86 shaders in parallel using `std::thread::scope`
/// with zero external crates.
#[forbid(unsafe_code)]
#[must_use]
pub fn compile_batch(inputs: &[OwnedCompileInput]) -> Vec<CompileOutcome> {
    if inputs.is_empty() {
        return Vec::new();
    }
    let worker_count = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
        .min(inputs.len())
        .max(1);
    if worker_count <= 1 || inputs.len() == 1 {
        return inputs
            .iter()
            .map(|item| compile_shader(&item.as_lower_input()))
            .collect();
    }

    let chunk_size = (inputs.len() + worker_count - 1) / worker_count;
    let mut results: Vec<Option<CompileOutcome>> = vec![None; inputs.len()];

    std::thread::scope(|scope| {
        let mut remaining_inputs = inputs;
        let mut remaining_out = results.as_mut_slice();
        while !remaining_inputs.is_empty() {
            let take = chunk_size.min(remaining_inputs.len());
            let (in_chunk, rest_in) = remaining_inputs.split_at(take);
            let (out_chunk, rest_out) = remaining_out.split_at_mut(take);
            remaining_inputs = rest_in;
            remaining_out = rest_out;
            scope.spawn(move || {
                for (inp, dst) in in_chunk.iter().zip(out_chunk.iter_mut()) {
                    *dst = Some(compile_shader(&inp.as_lower_input()));
                }
            });
        }
    });

    results
        .into_iter()
        .map(|o| {
            o.unwrap_or_else(|| CompileOutcome {
                spirv_words: Vec::new(),
                fallback: Some(FallbackReason {
                    offset: 0,
                    opcode: 0,
                    reason: "missing worker output",
                }),
                updated_bindings: Bindings::default(),
                info: ShaderInfo::default(),
            })
        })
        .collect()
}
