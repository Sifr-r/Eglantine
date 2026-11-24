// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Print the Maxwell words that given Ampere instructions of a dumped program lower to.
// Build outside the repo: c++ -std=c++20 -O1 -w -I <repo>/src lowered_for.cpp -lfmt -o lowered_for
// Usage: lowered_for <dump.bin> <ampere byte offset hex>...   (tensor lowering on, so HMMA programs lower)
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
using namespace Shader::Ampere;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <dump.bin> <offset>...\n", argv[0]);
        return 1;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "failed to open %s\n", argv[1]);
        return 1;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    std::vector<Instruction> code(bytes.size() / sizeof(Instruction));
    if (!code.empty()) {
        std::memcpy(code.data(), bytes.data(), code.size() * sizeof(Instruction));
    }
    const auto lowered = LowerToMaxwell(code, LoweringOptions{.tensor = true});
    const auto& offsets = lowered.source_instruction_offsets;
    if (offsets.empty()) {
        std::printf("program did not lower\n");
        return 1;
    }
    int exit_code = 0;
    for (int arg = 2; arg < argc; ++arg) {
        errno = 0;
        char* parse_end = nullptr;
        const unsigned long long offset = std::strtoull(argv[arg], &parse_end, 16);
        if (errno == ERANGE || parse_end == argv[arg] || *parse_end != '\0' ||
            std::strchr(argv[arg], '-') != nullptr ||
            offset > std::numeric_limits<u32>::max()) {
            std::fprintf(stderr, "invalid hex offset: %s\n", argv[arg]);
            exit_code = 1;
            continue;
        }
        const u32 index = static_cast<u32>(offset) / 16;
        if (index >= offsets.size() || index >= code.size()) continue;
        const size_t begin = offsets[index];
        const size_t end = index + 1 < offsets.size() ? offsets[index + 1] : lowered.instructions.size();
        std::printf("ampere %#x (%08x %08x %08x %08x) -> maxwell [%zu, %zu)\n", index * 16, code[index].words[0],
                    code[index].words[1], code[index].words[2], code[index].words[3], begin, end);
        for (size_t i = begin; i < end; ++i)
            std::printf("   %016llx\n", static_cast<unsigned long long>(lowered.instructions[i]));
    }
    return exit_code;
}
