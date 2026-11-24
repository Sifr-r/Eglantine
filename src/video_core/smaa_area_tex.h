// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Jorge Jimenez (jorge@iryoku.com)
// SPDX-FileCopyrightText: 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
// SPDX-FileCopyrightText: 2013 Belen Masia (bmasia@unizar.es)
// SPDX-FileCopyrightText: 2013 Fernando Navarro (fernandn@microsoft.com)
// SPDX-FileCopyrightText: 2013 Diego Gutierrez (diegog@unizar.es)
// SPDX-License-Identifier: MIT

#pragma once

#define AREATEX_WIDTH 160
#define AREATEX_HEIGHT 560
#define AREATEX_PITCH (AREATEX_WIDTH * 2)
#define AREATEX_SIZE (AREATEX_HEIGHT * AREATEX_PITCH)

/**
 * Stored in R8G8 format. Load it in the following format:
 *  - DX9:  D3DFMT_A8L8
 *  - DX10: DXGI_FORMAT_R8G8_UNORM
 */
extern const unsigned char areaTexBytes[AREATEX_SIZE];
