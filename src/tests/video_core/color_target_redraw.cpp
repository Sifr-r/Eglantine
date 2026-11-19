// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/texture_cache/image_base.h"

namespace {

using VideoCommon::ImageBase;
using VideoCommon::NullImageParams;

constexpr u64 current_frame = 10;

} // namespace

TEST_CASE("ImageBase: a target written this frame is redrawn", "[nx2_memory]") {
    ImageBase image{NullImageParams{}};
    image.first_written_frame = current_frame;
    image.last_written_frame = current_frame;
    REQUIRE(image.RedrawnEveryFrame(current_frame));
}

TEST_CASE("ImageBase: a target written last frame is redrawn", "[nx2_memory]") {
    ImageBase image{NullImageParams{}};
    image.first_written_frame = current_frame - 1;
    image.last_written_frame = current_frame - 1;
    REQUIRE(image.RedrawnEveryFrame(current_frame));
}

TEST_CASE("ImageBase: a target not written for two frames is not redrawn", "[nx2_memory]") {
    ImageBase image{NullImageParams{}};
    image.first_written_frame = 1;
    image.last_written_frame = current_frame - 2;
    REQUIRE_FALSE(image.RedrawnEveryFrame(current_frame));
}

TEST_CASE("ImageBase: a target cleared this frame is not redrawn", "[nx2_memory]") {
    ImageBase image{NullImageParams{}};
    image.first_written_frame = current_frame - 1;
    image.last_written_frame = current_frame - 1;
    image.cleared_frame = current_frame;
    REQUIRE_FALSE(image.RedrawnEveryFrame(current_frame));
}

TEST_CASE("ImageBase: a target never written is not redrawn", "[nx2_memory]") {
    ImageBase image{NullImageParams{}};
    REQUIRE_FALSE(image.RedrawnEveryFrame(current_frame));
}
