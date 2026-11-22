#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2025 crueter
# SPDX-License-Identifier: GPL-3.0-or-later

# do NOT use fd in scripts, PLEASE
if [ "$#" -gt 0 ]; then
    exec shellcheck "$@"
fi
find tools -name "*.sh" -exec shellcheck {} +
