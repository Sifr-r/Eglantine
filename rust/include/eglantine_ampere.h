// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EglAmpereInstruction {
    uint32_t words[4];
} EglAmpereInstruction;

typedef enum EglAmpereCaptureResult {
    EGL_AMPERE_CAPTURE_CONTINUE = 0,
    EGL_AMPERE_CAPTURE_COMPLETE = 1,
    EGL_AMPERE_CAPTURE_INVALID_TARGET = 2,
} EglAmpereCaptureResult;

typedef struct EglAmpereCaptureState {
    EglAmpereInstruction exit_setup[5];
    uint32_t exit_handler_end;
    uint32_t last_call_target;
    uint32_t last_forward_target;
    uint8_t main_exited;
    uint8_t computed_branch;
    uint8_t _pad[2];
} EglAmpereCaptureState;

/// Options controlling SM86 -> Maxwell lowering.
/// `lower_extended_override`:
///   < 0 : read EDEN_NX2_LOWER_EXTENDED environment variable (default C++ behavior)
///     0 : force extended lowering off
///   > 0 : force extended lowering on
typedef struct EglAmpereLoweringOptions {
    uint8_t geometry;
    uint8_t tensor;
    uint8_t compact_tensor;
    int8_t lower_extended_override;
    uint8_t _pad[4];
    uint64_t native_code_address;
} EglAmpereLoweringOptions;

typedef struct EglAmpereLoweredProgram {
    uint64_t* instructions;
    size_t instruction_count;
    uint32_t* source_instruction_offsets;
    size_t source_instruction_offset_count;
    uint32_t source_size;
    uint32_t required_texture_buffer;
    uint32_t high_uniform_registers;
    uint32_t error_offset;
    char* error;
    size_t error_len;
} EglAmpereLoweredProgram;

typedef struct EglAmpereBytes {
    uint8_t* data;
    size_t len;
} EglAmpereBytes;

void egl_ampere_capture_init(EglAmpereCaptureState* state);

EglAmpereCaptureResult egl_ampere_capture_observe(EglAmpereCaptureState* state,
                                                  const EglAmpereInstruction* insn,
                                                  uint32_t index);

uint64_t egl_ampere_native_program_key(const EglAmpereInstruction* source, size_t count);

uint8_t egl_ampere_has_native_address_map(const uint64_t* code, size_t count);

uint8_t egl_ampere_native_program_address(const uint64_t* code,
                                          size_t count,
                                          uint32_t address,
                                          uint32_t* out_target);

uint32_t egl_ampere_override_source_size(const uint8_t* bytes, size_t len);

/// Returns 0 on success (empty `out_bytes` if the program fails validation), non-zero on invalid args/panic.
int32_t egl_ampere_encode_override(const EglAmpereInstruction* source,
                                   size_t source_count,
                                   const EglAmpereLoweredProgram* program,
                                   EglAmpereBytes* out_bytes);

/// Returns 0 on success (check `out_program->error_len == 0` for program validity), non-zero on invalid args/panic.
int32_t egl_ampere_decode_override(const uint8_t* bytes,
                                   size_t len,
                                   const EglAmpereInstruction* source,
                                   size_t source_count,
                                   EglAmpereLoweredProgram* out_program);

/// Returns 0 on success (check `out_program->error_len == 0` for lowering success), non-zero on invalid args/panic.
int32_t egl_ampere_lower_to_maxwell(const EglAmpereInstruction* code,
                                    size_t count,
                                    const EglAmpereLoweringOptions* options,
                                    EglAmpereLoweredProgram* out_program);

/// Idempotent: frees owned buffers and zeroes fields.
void egl_ampere_free_lowered(EglAmpereLoweredProgram* program);

/// Idempotent: frees owned bytes and zeroes fields.
void egl_ampere_free_bytes(EglAmpereBytes* bytes);

#ifdef __cplusplus
} // extern "C"
#endif
