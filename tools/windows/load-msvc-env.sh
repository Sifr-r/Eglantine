#!/usr/bin/bash
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

_msvc_sourced=0
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    _msvc_sourced=1
fi

ARCH_RAW="${PROCESSOR_ARCHITECTURE:-}"

case "$ARCH_RAW" in
    AMD64) ARCH="x64" ;;
    ARM64) ARCH="arm64" ;;
    *)
        echo "load-msvc-env.sh: Unsupported architecture: $ARCH_RAW" >&2
        if [ "$_msvc_sourced" -eq 1 ]; then
            unset _msvc_sourced ARCH_RAW
            return 1
        fi
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCVARS_BASH="$SCRIPT_DIR/vcvarsall.sh"

if [ ! -f "$VCVARS_BASH" ]; then
    echo "load-msvc-env.sh: vcvarsall.sh not found in $SCRIPT_DIR" >&2
    if [ "$_msvc_sourced" -eq 1 ]; then
        unset _msvc_sourced ARCH_RAW ARCH SCRIPT_DIR VCVARS_BASH
        return 1
    fi
    exit 1
fi

if ! env_script="$(bash "$VCVARS_BASH" "$ARCH")"; then
    echo "load-msvc-env.sh: vcvarsall.sh failed for $ARCH" >&2
    if [ "$_msvc_sourced" -eq 1 ]; then
        unset _msvc_sourced ARCH_RAW ARCH SCRIPT_DIR VCVARS_BASH env_script
        return 1
    fi
    exit 1
fi

if [ -z "$env_script" ]; then
    echo "load-msvc-env.sh: vcvarsall.sh produced no environment exports for $ARCH" >&2
    if [ "$_msvc_sourced" -eq 1 ]; then
        unset _msvc_sourced ARCH_RAW ARCH SCRIPT_DIR VCVARS_BASH env_script
        return 1
    fi
    exit 1
fi

if [ "$_msvc_sourced" -eq 1 ]; then
    eval "$env_script"
    echo "MSVC environment loaded for $ARCH with vcvars-bash script"
else
    printf '%s\n' "$env_script"
fi
unset _msvc_sourced ARCH_RAW ARCH SCRIPT_DIR VCVARS_BASH env_script