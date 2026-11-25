# Phase 3 — SIMD kernels, GPU and kernel hot paths, and ARM64 backends

This document records the original Phase 3 experiments. The [NX2 investigation notes](../NX2_NOTES.md) and [GPU thread-split measurements](../NX2_GPU_THREAD_SPLIT.md) describe specific runs. Use the [baseline procedure](00_PREREQUISITES_AND_BASELINE.md) on the target build, workload and hardware before choosing a hot path.

## 3A. Evaluate ISPC kernels

**Status:** The [ISPC build](../src/simd_kernels/CMakeLists.txt) contains ETC2/EAC decode, ASTC opacity scanning and texture swizzle. Each has a C++ path. The uncalled audio kernel was removed. Use the [ownership ledger](../ARCHITECTURE.md#first-party-language-and-domain-ownership) for the texture parity and throughput requirements.

The original evaluation included BC compression and audio mixing, filtering and resampling. For any future experiment, select ISAs for the target architecture and use C-compatible inputs and outputs. Exercise unaligned buffers, zero length, partial blocks and SIMD tails. Include state carried across calls for audio experiments.

Use the same original input and independent output/state for each comparison. Require exact output for lossless transforms. Define acceptable error for lossy compression or resampling before benchmarking. Record throughput with and without dispatch overhead. Disable the kernel if parity fails or the measured workload regresses.

## 3B. Reduce Vulkan draw setup and readbacks

**Status:** The [descriptor buffer ring](../src/video_core/renderer_vulkan/vk_descriptor_buffer.h) exists. Use the [architecture ledger](../ARCHITECTURE.md#video_core--the-gpu) to locate the current draw and readback paths. Measure descriptor writes and draw setup time before pursuing further cache changes. A reusable slice must still belong to a live ring allocation and match the layout, resource generations and command-buffer binding state. Exercise ring wrap, resource replacement, new command buffers and pipeline switches in parity runs.

Evaluate download interval coalescing separately in the buffer cache. A merged or skipped download must preserve the ordering of observable CPU, GPU and macro reads. Compare readback bytes and volume, frame times and draw setup cost against the existing path. Keep each optimization independently switchable during rollout.

## 3C. Investigate scheduler contention and frame pacing

**Proposal:** Instrument address waits and signals before changing the [scheduler lock](../src/core/hle/kernel/k_scheduler_lock.h). A fast path or sharded wait queue needs a protocol that makes the predicate check, waiter registration and wake decision consistent. Include wake ordering, timeout, cancellation and concurrent signal/wait in the stress cases.

Treat the reported frames-ahead deadlock as a reproduction target. Trace frame submission, syncpoint thresholds, fence completion and buffer release across a scene change. Test a bounded submission design against synchronous presentation, including teardown with outstanding work. The [swapchain](../src/video_core/renderer_vulkan/vk_swapchain.cpp) already selects mailbox, FIFO and relaxed FIFO where supported. Verify those modes and unsupported-mode fallback before attributing a pacing change to VRR.

## 3D. Evaluate ARM64 execution backends

**Status:** The [Turing macro JIT](../src/video_core/macro_turing_jit.h) has an oaknut AArch64 backend. Use the [native macro tests](../src/tests/video_core/native_macro_fifo.cpp) with `[native_macro]`. Compare registers, memory effects and emitted method batches against the interpreter on the same synthetic programs.

**Proposal:** Evaluate an Apple Silicon Hypervisor.framework backend separately behind an opt-in build option. Verify entitlement and VM creation at runtime, guest exception handling and memory visibility before performance testing. Exercise the existing CPU backend with the option disabled and when VM creation fails. Record host architecture and backend selection alongside each result.
