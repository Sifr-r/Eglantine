#!/bin/sh

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

# specify full path if dupes may exist
EXCLUDE_FILES="CPM.cmake CPMUtil.cmake GetSCMRev.cmake renderdoc_app.h tools/cpm tools/shellcheck.sh tools/update-cpm.sh tools/windows/vcvarsall.sh externals/stb externals/glad externals/getopt externals/gamemode externals/FidelityFX-SDK externals/demangle externals/bc_decoder externals/cmake-modules"

YEAR=$(date "+%Y")
HOLDER="Eglantine Emulator Project"
LICENSE="GPL-3.0-or-later"

UPDATE=false
COMMIT=false
BASE_REF="nx2"
BASE_EXPLICIT=false
TMP_DIR=""

cleanup() {
    if [ -n "${TMP_DIR:-}" ] && [ -d "$TMP_DIR" ]; then
        rm -rf "$TMP_DIR"
    fi
}
trap cleanup EXIT INT TERM

usage() {
    cat << EOF
Usage: $0 [options] [--] [file ...]
Compares the current HEAD to the base branch (default: nx2), or inspects the
given files, to verify SPDX license headers.

Options:
    --check             Check license headers and exit non-zero on failures (default)
    -u, --update        Fix license headers in place when missing or malformed
    -c, --commit        Commit updated files to Git (implies --update)
    -b, --base <ref>    Git base ref for diff mode (default: nx2, fallback: origin/nx2)
    -h, --help          Show this message and exit

Copyright $YEAR $HOLDER
Licensed under $LICENSE

Excluded paths/patterns:
EOF

    for excl in $EXCLUDE_FILES; do
        printf -- '- %s\n' "$excl"
    done
    exit 0
}

TMP_DIR=$(mktemp -d)
TARGET_LIST="$TMP_DIR/targets"
SRC_LIST="$TMP_DIR/src_files"
OTHER_LIST="$TMP_DIR/other_files"
: > "$TARGET_LIST"
: > "$SRC_LIST"
: > "$OTHER_LIST"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --check)
            UPDATE=false
            shift
            ;;
        -u|--update)
            UPDATE=true
            shift
            ;;
        -c|--commit)
            UPDATE=true
            COMMIT=true
            shift
            ;;
        -b|--base)
            [ "$#" -ge 2 ] || {
                echo "--base requires a ref argument" >&2
                exit 2
            }
            BASE_REF="$2"
            BASE_EXPLICIT=true
            shift 2
            ;;
        --base=*)
            BASE_REF=${1#--base=}
            BASE_EXPLICIT=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                printf '%s\n' "$1" >> "$TARGET_LIST"
                shift
            done
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
        *)
            printf '%s\n' "$1" >> "$TARGET_LIST"
            shift
            ;;
    esac
done

header_line1() {
    printf '%s SPDX-FileCopyrightText: Copyright %s %s\n' "$1" "$YEAR" "$HOLDER"
}

header_line2() {
    printf '%s SPDX-License-Identifier: %s\n' "$1" "$LICENSE"
}

header() {
    header_line1 "$1"
    header_line2 "$1"
}

comment_prefix_for_file() {
    case "$(basename -- "$1")" in
        *.cmake|*.sh|*.ps1|*.py|*.rb|*.perl|*.pl|*.nix|*CMakeLists.txt)
            printf '#'
            ;;
        *.kt|*.kts|*.cpp|*.h|*.qml|*.c|*.hpp|*.hxx|*.cxx|*.h.in|*.inc)
            printf '//'
            ;;
        *)
            return 1
            ;;
    esac
}

is_excluded() {
    for pattern in $EXCLUDE_FILES; do
        case "$1" in
            *"$pattern"*)
                return 0
                ;;
        esac
    done
    return 1
}

check_header() {
    begin="$1"
    file="$2"

    if awk -v begin="$begin" -v year="$YEAR" -v holder="$HOLDER" -v license="$LICENSE" '
        BEGIN {
            expected_cp = begin " SPDX-FileCopyrightText: Copyright " year " " holder
            expected_lic = begin " SPDX-License-Identifier: " license
            spdx_cp_prefix = begin " SPDX-FileCopyrightText:"
            spdx_lic_prefix = begin " SPDX-License-Identifier:"
            in_header = 1
            in_c_comment = 0
            stanza_cp = 0
            stanza_lic = 0
            stanza_egl_cp = 0
            stanza_last_was_lic = 0
            has_eglantine_stanza = 0
            invalid = 0
            exit_early = 0
        }
        function reset_stanza() {
            if (stanza_cp > 0 && stanza_lic == 0) {
                invalid = 1
            }
            stanza_cp = 0
            stanza_lic = 0
            stanza_egl_cp = 0
            stanza_last_was_lic = 0
        }
        {
            line = $0
            sub(/\r$/, "", line)

            if (NR == 1 && line ~ /^#!/) {
                next
            }
            if (!in_header) {
                exit
            }

            if (in_c_comment) {
                if (line ~ /\*\//) {
                    in_c_comment = 0
                }
                next
            }

            if (line ~ /^[[:space:]]*$/) {
                reset_stanza()
                next
            }

            if (begin == "//" && line ~ /^[[:space:]]*\/\*/) {
                if (line !~ /\*\//) {
                    in_c_comment = 1
                }
                next
            }

            if (substr(line, 1, length(begin)) == begin) {
                has_inline_cp = (line ~ /SPDX-FileCopyrightText:|Copyright/)
                is_spdx_lic = (index(line, spdx_lic_prefix) == 1 || (has_inline_cp && line ~ /SPDX-License-Identifier:/))

                if (is_spdx_lic) {
                    if (stanza_last_was_lic && !has_inline_cp) {
                        invalid = 1
                    }
                    stanza_lic = 1
                    stanza_last_was_lic = 1
                    if (stanza_egl_cp && line == expected_lic) {
                        has_eglantine_stanza = 1
                    }
                }

                if (index(line, spdx_cp_prefix) == 1 || has_inline_cp) {
                    if (line == expected_cp) {
                        stanza_egl_cp = 1
                    }
                    stanza_cp = 1
                    stanza_last_was_lic = 0
                }
                next
            }

            reset_stanza()
            in_header = 0
            exit_early = 1
            exit
        }
        END {
            if (!exit_early) {
                reset_stanza()
            }
            if (has_eglantine_stanza && !invalid) {
                exit 0
            }
            exit 1
        }
    ' "$file"; then
        return 0
    fi

    case "$begin" in
        "//")
            printf '%s\n' "$file" >> "$SRC_LIST"
            ;;
        "#")
            printf '%s\n' "$file" >> "$OTHER_LIST"
            ;;
    esac
}

update_file_header() {
    begin="$1"
    file="$2"
    out="$TMP_DIR/updated"
    new_cp=$(header_line1 "$begin")
    new_lic=$(header_line2 "$begin")

    IFS= read -r first_line < "$file" || first_line=""
    has_shebang=false
    case "$first_line" in
        '#!'*)
            has_shebang=true
            ;;
    esac

    if awk -v begin="$begin" -v holder="$HOLDER" '
        BEGIN { in_header = 1; found = 0 }
        {
            line = $0
            sub(/\r$/, "", line)
            if (NR == 1 && line ~ /^#!/) { next }
            if (line ~ /^[[:space:]]*$/) { next }
            if (substr(line, 1, length(begin)) == begin) {
                if (index(line, begin " SPDX-FileCopyrightText:") == 1 && index(line, holder) > 0) {
                    found = 1
                    exit
                }
                next
            }
            exit
        }
        END { exit(found ? 0 : 1) }
    ' "$file"; then
        awk -v begin="$begin" -v holder="$HOLDER" -v repl="$new_cp" '
            BEGIN { replaced = 0 }
            {
                if (!replaced && index($0, begin " SPDX-FileCopyrightText:") == 1 && index($0, holder) > 0) {
                    print repl
                    replaced = 1
                } else {
                    print $0
                }
            }
        ' "$file" > "$out"
    elif awk -v expected_lic="$new_lic" '
        {
            line = $0
            sub(/\r$/, "", line)
            if (NR == 1 && line ~ /^#!/) { next }
            if (line ~ /^[[:space:]]*$/) { next }
            if (line == expected_lic) { exit 0 }
            exit 1
        }
    ' "$file"; then
        awk -v new_cp="$new_cp" '
            BEGIN { inserted = 0 }
            {
                line = $0
                sub(/\r$/, "", line)
                if (!inserted && !(NR == 1 && line ~ /^#!/) && !(line ~ /^[[:space:]]*$/)) {
                    print new_cp
                    inserted = 1
                }
                print $0
            }
        ' "$file" > "$out"
    else
        : > "$out"
        if [ "$has_shebang" = "true" ]; then
            printf '%s\n\n%s\n%s\n' "$first_line" "$new_cp" "$new_lic" > "$out"
            awk 'NR == 1 { next } NR == 2 && $0 ~ /^[[:space:]]*$/ { print ""; next } NR == 2 { print ""; print $0; next } { print $0 }' "$file" >> "$out"
        else
            printf '%s\n%s\n\n' "$new_cp" "$new_lic" > "$out"
            cat "$file" >> "$out"
        fi
    fi

    cat "$out" > "$file"
}

if [ ! -s "$TARGET_LIST" ]; then
    RESOLVED_REF=""
    if git rev-parse --verify "${BASE_REF}^{commit}" >/dev/null 2>&1; then
        RESOLVED_REF="$BASE_REF"
    elif [ "$BASE_EXPLICIT" = "false" ] && git rev-parse --verify "origin/${BASE_REF}^{commit}" >/dev/null 2>&1; then
        RESOLVED_REF="origin/${BASE_REF}"
    else
        echo "Error: could not resolve git base ref '$BASE_REF'" >&2
        exit 2
    fi

    BASE=$(git merge-base "$RESOLVED_REF" HEAD)
    git diff --name-only "$BASE" > "$TARGET_LIST"
fi

while IFS= read -r file; do
    [ -n "$file" ] || continue
    [ -f "$file" ] || continue

    if is_excluded "$file"; then
        continue
    fi

    if ! begin=$(comment_prefix_for_file "$file"); then
        continue
    fi

    check_header "$begin" "$file"
done < "$TARGET_LIST"

if [ ! -s "$SRC_LIST" ] && [ ! -s "$OTHER_LIST" ]; then
    echo "-- All good."
    exit 0
fi

echo

if [ -s "$SRC_LIST" ]; then
    echo "-- The following source files have incorrect license headers:"
    HEADER=$(header "//")
    while IFS= read -r file; do
        echo "-- * $file"
    done < "$SRC_LIST"

    cat << EOF

-- The following license header should be added to the start of these offending files:

=== BEGIN ===
$HEADER
===  END  ===

EOF
fi

if [ -s "$OTHER_LIST" ]; then
    echo "-- The following script, Nix, and CMake files have incorrect license headers:"
    HEADER=$(header "#")
    while IFS= read -r file; do
        echo "-- * $file"
    done < "$OTHER_LIST"

    cat << EOF

-- The following license header should be added to the start of these offending files:

=== BEGIN ===
$HEADER
===  END  ===

EOF
fi

if [ "$UPDATE" = "true" ]; then
    echo "-- Fixing headers..."

    for list_file in "$SRC_LIST" "$OTHER_LIST"; do
        [ -s "$list_file" ] || continue
        while IFS= read -r file; do
            [ -n "$file" ] || continue
            if ! begin=$(comment_prefix_for_file "$file"); then
                continue
            fi
            update_file_header "$begin" "$file"
            if [ "$COMMIT" = "true" ]; then
                git add "$file"
            fi
        done < "$list_file"
    done

    echo "-- Done"

    if [ "$COMMIT" = "true" ]; then
        echo "-- Committing changes"
        git commit -m "Fix license headers"
        echo "-- Changes committed. You may now push."
    fi

    exit 0
fi

exit 1
