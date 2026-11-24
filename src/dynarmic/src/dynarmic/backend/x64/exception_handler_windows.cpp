// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstring>
#include <vector>

#include "common/assert.h"
#include <bit>
#include "common/common_types.h"

#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/common/safe_ops.h"

using UBYTE = u8;

enum UNWIND_REGISTER_CODES {
    UWRC_RAX,
    UWRC_RCX,
    UWRC_RDX,
    UWRC_RBX,
    UWRC_RSP,
    UWRC_RBP,
    UWRC_RSI,
    UWRC_RDI,
    UWRC_R8,
    UWRC_R9,
    UWRC_R10,
    UWRC_R11,
    UWRC_R12,
    UWRC_R13,
    UWRC_R14,
    UWRC_R15,
};

enum UNWIND_OPCODE {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE = 1,
    UWOP_ALLOC_SMALL = 2,
    UWOP_SET_FPREG = 3,
    UWOP_SAVE_NONVOL = 4,
    UWOP_SAVE_NONVOL_FAR = 5,
    UWOP_SAVE_XMM128 = 8,
    UWOP_SAVE_XMM128_FAR = 9,
    UWOP_PUSH_MACHFRAME = 10,
};

union UNWIND_CODE {
    struct {
        UBYTE CodeOffset;
        UBYTE UnwindOp : 4;
        UBYTE OpInfo : 4;
    } code;
    USHORT FrameOffset;
};

// UNWIND_INFO is a tail-padded structure
struct UNWIND_INFO {
    UBYTE Version : 3;
    UBYTE Flags : 5;
    UBYTE SizeOfProlog;
    UBYTE CountOfCodes;
    UBYTE FrameRegister : 4;
    UBYTE FrameOffset : 4;
    // UNWIND_CODE UnwindCode[];
    // With Flags == 0 there are no additional fields.
    // OPTIONAL UNW_EXCEPTION_INFO ExceptionInfo;
};

struct UNW_EXCEPTION_INFO {
    ULONG ExceptionHandler;
    // OPTIONAL ARBITRARY HandlerData;
};

static_assert(sizeof(UNWIND_INFO) == 4);
static_assert(sizeof(UNWIND_CODE) == 2);
static_assert(sizeof(UNW_EXCEPTION_INFO) == 4);

namespace Dynarmic::Backend {

using namespace Dynarmic::Backend::X64;

struct PrologueInformation {
    std::vector<UNWIND_CODE> unwind_code;
    size_t number_of_unwind_code_entries;
    u8 prolog_size;
};

static PrologueInformation BuildPrologueInformation(const PrologueInfo& info, bool zero_prolog) {
    PrologueInformation ret;

    const auto next_entry = [&]() -> UNWIND_CODE& {
        ret.unwind_code.emplace_back();
        return ret.unwind_code.back();
    };
    const auto push_nonvol = [&](u8 offset, UNWIND_REGISTER_CODES reg) {
        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_PUSH_NONVOL;
        entry.code.OpInfo = reg;
    };
    const auto alloc_large = [&](u8 offset, size_t size) {
        ASSERT(size > 0 && size % 8 == 0);
        size /= 8;

        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_ALLOC_LARGE;
        if (size <= 0xFFFF) {
            entry.code.OpInfo = 0;
            auto& size_entry = next_entry();
            size_entry.FrameOffset = static_cast<USHORT>(size);
        } else {
            entry.code.OpInfo = 1;
            auto& size_entry_1 = next_entry();
            size_entry_1.FrameOffset = static_cast<USHORT>(size);
            auto& size_entry_2 = next_entry();
            size_entry_2.FrameOffset = static_cast<USHORT>(size >> 16);
        }
    };
    const auto save_xmm128 = [&](u8 offset, u8 reg, size_t frame_offset) {
        ASSERT(frame_offset % 16 == 0);
        ASSERT(frame_offset / 16 <= 0xFFFF);

        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_SAVE_XMM128;
        entry.code.OpInfo = reg;
        auto& offset_entry = next_entry();
        offset_entry.FrameOffset = static_cast<USHORT>(frame_offset / 16);
    };

    ret.prolog_size = zero_prolog ? u8{0} : info.prolog_size;
    for (auto it = info.ops.rbegin(); it != info.ops.rend(); ++it) {
        const u8 offset = zero_prolog ? u8{0} : it->code_offset;
        switch (it->kind) {
        case PrologueInfo::OpKind::PushNonVol:
            push_nonvol(offset, static_cast<UNWIND_REGISTER_CODES>(it->reg));
            break;
        case PrologueInfo::OpKind::AllocStack:
            alloc_large(offset, it->offset_or_size);
            break;
        case PrologueInfo::OpKind::SaveXmm128:
            save_xmm128(offset, it->reg, it->offset_or_size);
            break;
        }
    }

    ret.number_of_unwind_code_entries = ret.unwind_code.size();

    // The Windows API requires the size of the unwind_code array
    // to be a multiple of two for alignment reasons.
    if (ret.unwind_code.size() % 2 == 1) {
        auto& last_entry = next_entry();
        last_entry.FrameOffset = 0;
    }
    ASSERT(ret.unwind_code.size() % 2 == 0);

    return ret;
}

static DWORD EmitUnwindInfo(BlockOfCode& code, const PrologueInfo& info, ULONG handler_rva, bool zero_prolog) {
    const auto prolog_info = BuildPrologueInformation(info, zero_prolog);

    code.align(16);
    UNWIND_INFO* unwind_info = static_cast<UNWIND_INFO*>(code.AllocateFromCodeSpace(sizeof(UNWIND_INFO)));
    unwind_info->Version = 1;
    unwind_info->Flags = UNW_FLAG_EHANDLER;
    unwind_info->SizeOfProlog = prolog_info.prolog_size;
    unwind_info->CountOfCodes = static_cast<UBYTE>(prolog_info.number_of_unwind_code_entries);
    unwind_info->FrameRegister = 0;  // No frame register present
    unwind_info->FrameOffset = 0;    // Unused because FrameRegister == 0
    // UNWIND_INFO::UnwindCode field:
    const size_t size_of_unwind_code = sizeof(UNWIND_CODE) * prolog_info.unwind_code.size();
    UNWIND_CODE* unwind_code = static_cast<UNWIND_CODE*>(code.AllocateFromCodeSpace(size_of_unwind_code));
    std::memcpy(unwind_code, prolog_info.unwind_code.data(), size_of_unwind_code);
    // UNWIND_INFO::ExceptionInfo field:
    UNW_EXCEPTION_INFO* except_info = static_cast<UNW_EXCEPTION_INFO*>(code.AllocateFromCodeSpace(sizeof(UNW_EXCEPTION_INFO)));
    except_info->ExceptionHandler = handler_rva;

    return static_cast<DWORD>(reinterpret_cast<const u8*>(unwind_info) - code.getCode());
}

struct ExceptionHandler::Impl final {
    Impl(BlockOfCode& code) {
        code.align(16);
        const u8* const handler_stubs_begin = code.getCurr<const u8*>();

        exception_handler_without_cb = code.getCurr<const u8*>();
        code.mov(code.eax, u32(ExceptionContinueSearch));
        code.ret();

        code.align(16);
        exception_handler_with_cb = code.getCurr<const u8*>();
        // Our 3rd argument is a PCONTEXT.

        // If not within our codeblock, ignore this exception.
        code.mov(code.rax, Safe::Negate(std::bit_cast<u64>(code.getCode())));
        code.add(code.rax, code.qword[code.ABI_PARAM3 + Xbyak::RegExp(offsetof(CONTEXT, Rip))]);
        code.cmp(code.rax, static_cast<u32>(code.GetTotalCodeSize()));
        code.ja(exception_handler_without_cb);

        code.lea(code.rsp, code.ptr[code.rsp - (8 + ABI_SHADOW_SPACE)]);
        code.mov(code.ABI_PARAM1, std::bit_cast<u64>(&cb));
        code.mov(code.ABI_PARAM2, code.ABI_PARAM3);
        code.CallLambda([](const std::function<FakeCall(u64)>& cb_, PCONTEXT ctx) {
            FakeCall fc = cb_(ctx->Rip);
            ctx->Rsp -= sizeof(u64);
            *std::bit_cast<u64*>(ctx->Rsp) = fc.ret_rip;
            ctx->Rip = fc.call_rip;
        });
        code.add(code.rsp, static_cast<u32>(8 + ABI_SHADOW_SPACE));
        code.mov(code.eax, u32(ExceptionContinueExecution));
        code.ret();

        code.align(16);
        const u8* const exception_handler_trampoline = code.getCurr<const u8*>();
        code.mov(code.rax, std::bit_cast<u64>(&active_exception_handler));
        code.jmp(code.ptr[code.rax]);

        active_exception_handler = exception_handler_without_cb;

        const ULONG trampoline_rva = static_cast<ULONG>(exception_handler_trampoline - code.getCode());
        const auto& run_prologue = code.GetRunCodePrologueInfo();
        const auto& step_prologue = code.GetStepCodePrologueInfo();
        ASSERT(run_prologue.begin != nullptr && step_prologue.begin != nullptr);

        const DWORD unwind_run_rva = EmitUnwindInfo(code, run_prologue, trampoline_rva, false);
        const DWORD unwind_step_rva = EmitUnwindInfo(code, step_prologue, trampoline_rva, false);
        const DWORD unwind_jit_rva = EmitUnwindInfo(code, run_prologue, trampoline_rva, true);
        const u8* const handler_stubs_end = code.getCurr<const u8*>();

        const u8* const base = code.getCode();
        rfuncs[0].BeginAddress = static_cast<DWORD>(run_prologue.begin - base);
        rfuncs[0].EndAddress = static_cast<DWORD>(step_prologue.begin - base);
        rfuncs[0].UnwindData = unwind_run_rva;

        rfuncs[1].BeginAddress = static_cast<DWORD>(step_prologue.begin - base);
        rfuncs[1].EndAddress = static_cast<DWORD>(handler_stubs_begin - base);
        rfuncs[1].UnwindData = unwind_step_rva;

        rfuncs[2].BeginAddress = static_cast<DWORD>(handler_stubs_end - base);
        rfuncs[2].EndAddress = static_cast<DWORD>(code.GetTotalCodeSize());
        rfuncs[2].UnwindData = unwind_jit_rva;

        ASSERT(rfuncs[0].BeginAddress < rfuncs[0].EndAddress);
        ASSERT(rfuncs[0].EndAddress == rfuncs[1].BeginAddress);
        ASSERT(rfuncs[1].BeginAddress < rfuncs[1].EndAddress);
        ASSERT(rfuncs[1].EndAddress < rfuncs[2].BeginAddress);
        ASSERT(rfuncs[2].BeginAddress < rfuncs[2].EndAddress);

        RtlAddFunctionTable(rfuncs.data(), static_cast<DWORD>(rfuncs.size()), reinterpret_cast<DWORD64>(base));
    }

    void SetCallback(std::function<FakeCall(u64)> new_cb) {
        cb = std::move(new_cb);
        active_exception_handler = cb ? exception_handler_with_cb : exception_handler_without_cb;
    }

    ~Impl() {
        RtlDeleteFunctionTable(rfuncs.data());
    }

private:
    std::array<RUNTIME_FUNCTION, 3> rfuncs{};
    std::function<FakeCall(u64)> cb;
    const u8* active_exception_handler = nullptr;
    const u8* exception_handler_without_cb = nullptr;
    const u8* exception_handler_with_cb = nullptr;
};

ExceptionHandler::ExceptionHandler() = default;
ExceptionHandler::~ExceptionHandler() = default;

void ExceptionHandler::Register(BlockOfCode& code) {
    impl = std::make_unique<Impl>(code);
}

bool ExceptionHandler::SupportsFastmem() const noexcept {
    return static_cast<bool>(impl);
}

void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {
    impl->SetCallback(cb);
}

}  // namespace Dynarmic::Backend
