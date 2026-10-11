#!/bin/sh
# Embeds src/prelude.graphqls as a C string literal (src/prelude.inc).
set -e
in="${1:-src/prelude.graphqls}"
out="${2:-src/prelude.inc}"
{
    echo '/* generated from prelude.graphqls by scripts/embed_prelude.sh; do not edit */'
    echo 'static const char GQL_PRELUDE[] ='
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/    "/' -e 's/$/\\n"/' "$in"
    echo '    ;'
} > "$out"
