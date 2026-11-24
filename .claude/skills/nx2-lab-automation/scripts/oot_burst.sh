#!/bin/zsh

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Load the save, leave the camera alone, save every presented frame for a while. Usage: oot_burst.sh <tag> <frames> [launch args]
if [ $# -lt 2 ] || [ -z "$1" ] || [ -z "$2" ]; then
  echo "Usage: oot_burst.sh <tag> <frames> [launch args]" >&2
  exit 1
fi
case "$2" in
  ''|*[!0-9]*)
    echo "Usage: oot_burst.sh <tag> <frames> [launch args] (frames must be a positive integer)" >&2
    exit 1
    ;;
esac
if [ "$2" -le 0 ]; then
  echo "Usage: oot_burst.sh <tag> <frames> [launch args] (frames must be > 0)" >&2
  exit 1
fi
TAG=$1; N=$2; shift 2
cd "${REPO:-/home/kraibse/clones/Eglantine}" || exit 1
D=${D:-${TMPDIR:-/tmp}/ootlab}
B=$D/burst_$TAG
rm -rf $B; mkdir -p $B
L=(python3 -I tools/nx2/lab_ctl.py -d $D)
if ! D="$D" zsh .claude/skills/nx2-lab-automation/scripts/to_gameplay.sh --max-seconds 300 --env EDEN_NX2_FRAME_CAPTURE="$B" "$@" > /tmp/oot_burst_boot.log 2>&1; then
  echo "to_gameplay.sh failed; see /tmp/oot_burst_boot.log" >&2
  $L stop 2>&1 | tail -1
  exit 1
fi
sleep 5
# PRE="<scancode> <ms>;..." turns the camera/walks before the burst (I/J/K/L = 12/13/14/15 camera).
for k in ${(s:;:)PRE}; do echo "$k" > $D/keys.txt; sleep $(( ${k#* } / 1000.0 + 0.8 )); done
[ -n "$PRE" ] && $L capture --settle 2 --out $D/burst_${TAG}_view.png > /dev/null 2>&1
rm -f $B/nx2-frame-*.tga(N)
echo $N > $B/burst
start=$(date +%s)
while [ -f $B/burst ] && [ $(( $(date +%s) - start )) -lt 10 ]; do sleep 0.5; done
sleep $(( N / 20 + 5 ))
cp $D/emulator.log /tmp/burst_$TAG.emulator.log
$L stop 2>&1 | tail -1
frames=($B/nx2-frame-*.tga(N))
echo "frames: ${#frames}"
