#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

ANDROID=src/android/app/src/main
STRINGS=$ANDROID/res/values/strings.xml

TMP_DIR=$(mktemp -d)

USED="$TMP_DIR"/used
UNUSED="$TMP_DIR"/unused
FILTERED="$TMP_DIR"/filtered
ALL_STRINGS="$TMP_DIR"/all

# First we find what files to search and what strings are defined
find $ANDROID -type f -iname '*.xml' -a -not -iname '*strings.xml' -o -iname '*.kt' | grep -v drawable >"$TMP_DIR"/files
grep -e "string name" $STRINGS | cut -d'"' -f2 >"$ALL_STRINGS"

# then get a list of all strings that are actually used
set +e
while IFS= read -r file; do
    grep -o 'R\.string\.[a-zA-Z0-9_]\+' "$file" >> "$USED"
    grep -o '@string/[a-zA-Z0-9_]\+' "$file" >> "$USED"
done <"$TMP_DIR"/files
set -e

# filter out "@string/" and "R.string." from the strings to get the raw names
sed 's/R.string.//' "$USED" | sed 's/@string\///' \
    | sort -u | grep -v app_name_suffixed > "$FILTERED"

# now we run a sort + uniq -u pass - this basically removes all strings that are
# present in BOTH the used strings list AND strings.xml
# thus giving us strings that are either ONLY used in strings.xml OR in the files

# NB: This also gives strings that may be found in comments but nowhere else,
# as well as some android builtins, but sed-ing those out of strings.xml is a no-op

# e.g. appbar_scrolling_view_behavior
cat "$FILTERED" "$ALL_STRINGS" | sort | uniq -u > "$UNUSED"

# finally, print out all unused strings and remove them from ALL strings.xml definitions
if [ -s "$UNUSED" ]; then
    cat "$UNUSED"
    find "$ANDROID/res" -iname '*strings.xml' -exec python3 -I -c '
import sys
import xml.parsers.expat as expat

def remove_unused(path, unused):
    with open(path, "rb") as fh:
        data = fh.read()
    parser = expat.ParserCreate()
    spans = []
    cur_start = None
    depth = 0

    def on_start(name, attrs):
        nonlocal cur_start, depth
        if cur_start is not None:
            depth += 1
        elif name == "string" and attrs.get("name") in unused:
            cur_start = parser.CurrentByteIndex
            depth = 1

    def on_end(name):
        nonlocal cur_start, depth
        if cur_start is None:
            return
        depth -= 1
        if depth == 0:
            idx = parser.CurrentByteIndex
            end = data.index(b">", idx) + 1 if data.startswith(b"</", idx) else idx
            s, e = cur_start, end
            ls = s
            while ls > 0 and data[ls - 1:ls] in (b" ", b"\t"):
                ls -= 1
            if ls == 0 or data[ls - 1:ls] == b"\n":
                re_idx = e
                while re_idx < len(data) and data[re_idx:re_idx + 1] in (b" ", b"\t"):
                    re_idx += 1
                if data[re_idx:re_idx + 2] == b"\r\n":
                    s, e = ls, re_idx + 2
                elif data[re_idx:re_idx + 1] == b"\n":
                    s, e = ls, re_idx + 1
                elif re_idx == len(data):
                    s, e = ls, re_idx
            spans.append((s, e))
            cur_start = None

    parser.StartElementHandler = on_start
    parser.EndElementHandler = on_end
    parser.Parse(data, True)
    if spans:
        out = bytearray()
        pos = 0
        for s, e in spans:
            if s > pos:
                out.extend(data[pos:s])
            pos = e
        out.extend(data[pos:])
        with open(path, "wb") as fh:
            fh.write(out)

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    unused = {line.rstrip("\r\n") for line in fh if line.strip()}

for path in sys.argv[2:]:
    remove_unused(path, unused)
' "$UNUSED" {} +
fi
