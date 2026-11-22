# Phase 3 — SIMD kernels, GPU and kernel hot paths, and ARM64 backends

These are proposed experiments. The [NX2 investigation notes](../NX2_NOTES.md) and [GPU thread-split measurements](../NX2_GPU_THREAD_SPLIT.md) describe specific runs. Capture a baseline on the target build, workload and hardware before choosing a hot path.

## 3A. Evaluate ISPC kernels

Start with one texture or audio operation that contributes measurable CPU time. Candidates include ETC2/EAC decode, ASTC opacity scans, BC compression, GOB swizzle and audio mixing, filtering or resampling.

Add an opt-in ISPC build in CMake and a host compiler in the Nix package. Select targets for the **target** architecture. Keep the existing C++ implementation available on every build, including machines lacking a compiled ISA. Expose ordinary C-compatible inputs and outputs to the caller. Test unaligned buffers, zero length, partial blocks, SIMD tails and audio state carried across calls.

Use the same original input and independent output/state for each comparison. Require exact output for lossless transforms. Define acceptable error for lossy compression or resampling before benchmarking. Record throughput with and without dispatch overhead. Disable the kernel if parity fails or the measured workload regresses.

## 3B. Reduce Vulkan draw setup and readbacks

The [descriptor buffer ring](../src/video_core/renderer_vulkan/vk_descriptor_buffer.h) already exists. Measure descriptor writes and draw setup time in the current renderer before adding a per-stage cache. A reusable slice must still belong to a live ring allocation and match the layout, resource generations and command-buffer binding state. Exercise ring wrap, resource replacement, new command buffers and pipeline switches in parity runs.

Evaluate download interval coalescing separately in the buffer cache. A merged or skipped download must preserve the ordering of observable CPU, GPU and macro reads. Compare readback bytes and volume, frame times and draw setup cost against the existing path. Keep each optimization independently switchable during rollout.

## 3C. Investigate scheduler contention and frame pacing

Instrument address waits and signals before changing the [scheduler lock](../src/core/hle/kernel/k_scheduler_lock.h). A fast path or sharded wait queue needs a protocol that makes the predicate check, waiter registration and wake decision consistent. Include wake ordering, timeout, cancellation and concurrent signal/wait in the stress cases.

Treat the reported frames-ahead deadlock as a reproduction target. Trace frame submission, syncpoint thresholds, fence completion and buffer release across a scene change. Test a bounded submission design against synchronous presentation, including teardown with outstanding work. The [swapchain](../src/video_core/renderer_vulkan/vk_swapchain.cpp) already selects mailbox, FIFO and relaxed FIFO where supported. Verify those modes and unsupported-mode fallback before attributing a pacing change to VRR.

## 3D. Evaluate ARM64 execution backends

Port the [Turing macro JIT](../src/video_core/macro_turing_jit.h) as an independent experiment using oaknut on AArch64. Compare registers, memory effects and emitted method batches against the interpreter on the same synthetic programs. Retain interpreter selection for unsupported instructions and for rollback.

Evaluate an Apple Silicon Hypervisor.framework backend separately behind an opt-in build option. Verify entitlement and VM creation at runtime, guest exception handling and memory visibility before performance testing. Exercise the existing CPU backend with the option disabled and when VM creation fails. Record host architecture and backend selection alongside each result.
