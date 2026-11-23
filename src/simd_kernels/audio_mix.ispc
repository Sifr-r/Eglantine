// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Multi-ISA fixed-point audio mixing and volume scaling kernels matching Common::FixedPoint<64-Q, Q>.

static inline int32 fixed_to_int_q15(int64 raw) {
    const uniform int64 FRAC_MASK = (((int64)1) << 15) - 1;
    const uniform int64 INT_MASK = ~FRAC_MASK;
    const int64 rounded = raw + ((raw & FRAC_MASK) >> 1);
    return (int32)((rounded & INT_MASK) >> 15);
}

static inline int32 fixed_to_int_q23(int64 raw) {
    const uniform int64 FRAC_MASK = (((int64)1) << 23) - 1;
    const uniform int64 INT_MASK = ~FRAC_MASK;
    const int64 rounded = raw + ((raw & FRAC_MASK) >> 1);
    return (int32)((rounded & INT_MASK) >> 23);
}

export void audio_mix_apply_q15(uniform int32 output[], const uniform int32 input[],
                                uniform int64 volume_raw, uniform uint32 sample_count) {
    foreach (i = 0 ... sample_count) {
        const int64 prod = (int64)input[i] * volume_raw;
        const int64 sum = ((int64)output[i] << 15) + prod;
        output[i] = fixed_to_int_q15(sum);
    }
}

export void audio_mix_apply_q23(uniform int32 output[], const uniform int32 input[],
                                uniform int64 volume_raw, uniform uint32 sample_count) {
    foreach (i = 0 ... sample_count) {
        const int64 prod = (int64)input[i] * volume_raw;
        const int64 sum = ((int64)output[i] << 23) + prod;
        output[i] = fixed_to_int_q23(sum);
    }
}

export uniform int32 audio_mix_apply_ramp_q15(uniform int32 output[], const uniform int32 input[],
                                            uniform int64 volume_raw, uniform int64 ramp_raw,
                                            uniform uint32 sample_count) {
    if (sample_count == 0u) {
        return 0;
    }
    foreach (i = 0 ... sample_count) {
        const int64 vol = volume_raw + (int64)i * ramp_raw;
        const int64 sample = (int64)input[i] * vol;
        const int64 sum = ((int64)output[i] << 15) + sample;
        output[i] = fixed_to_int_q15(sum);
    }
    const uniform uint32 last = sample_count - 1u;
    const uniform int64 last_vol = volume_raw + (uniform int64)last * ramp_raw;
    const uniform int64 last_sample = (uniform int64)input[last] * last_vol;
    const uniform int64 frac_mask = (((uniform int64)1) << 15) - 1;
    const uniform int64 rounded = last_sample + ((last_sample & frac_mask) >> 1);
    return (uniform int32)((rounded & ~frac_mask) >> 15);
}

export uniform int32 audio_mix_apply_ramp_q23(uniform int32 output[], const uniform int32 input[],
                                            uniform int64 volume_raw, uniform int64 ramp_raw,
                                            uniform uint32 sample_count) {
    if (sample_count == 0u) {
        return 0;
    }
    foreach (i = 0 ... sample_count) {
        const int64 vol = volume_raw + (int64)i * ramp_raw;
        const int64 sample = (int64)input[i] * vol;
        const int64 sum = ((int64)output[i] << 23) + sample;
        output[i] = fixed_to_int_q23(sum);
    }
    const uniform uint32 last = sample_count - 1u;
    const uniform int64 last_vol = volume_raw + (uniform int64)last * ramp_raw;
    const uniform int64 last_sample = (uniform int64)input[last] * last_vol;
    const uniform int64 frac_mask = (((uniform int64)1) << 23) - 1;
    const uniform int64 rounded = last_sample + ((last_sample & frac_mask) >> 1);
    return (uniform int32)((rounded & ~frac_mask) >> 23);
}

export void audio_volume_apply_q15(uniform int32 output[], const uniform int32 input[],
                                   uniform int64 gain_raw, uniform uint32 sample_count) {
    foreach (i = 0 ... sample_count) {
        output[i] = fixed_to_int_q15((int64)input[i] * gain_raw);
    }
}

export void audio_volume_apply_q23(uniform int32 output[], const uniform int32 input[],
                                   uniform int64 gain_raw, uniform uint32 sample_count) {
    foreach (i = 0 ... sample_count) {
        output[i] = fixed_to_int_q23((int64)input[i] * gain_raw);
    }
}
