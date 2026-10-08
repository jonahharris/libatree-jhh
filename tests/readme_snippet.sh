#!/bin/sh
# Compiles and runs the first ```c block of README.md so the front-page
# example cannot drift from the API. The block is a fragment: #include lines,
# then declarations and statements. It becomes a program by emitting the
# includes, then everything else inside main().
#
# usage: tests/readme_snippet.sh BUILD_DIR CC STLIB [LDFLAGS]
#   CC may carry flags ("clang -fsanitize=address"); it is word-split.
set -u
BUILD="$1"
CC="$2"
STLIB="$3"
LDFLAGS="${4:-}"
DIR=$(dirname "$0")
README="$DIR/../README.md"
SRC="$BUILD/tests/readme_snippet.c"
BIN="$BUILD/tests/readme_snippet"

mkdir -p "$BUILD/tests"
awk '
    !found && /^```c[ \t]*$/ { found = 1; inblock = 1; next }
    inblock && /^```/        { inblock = 0; exit }
    inblock && /^#include/   { inc[ni++] = $0; next }
    inblock                  { body[nb++] = $0 }
    END {
        if (!found || ni + nb == 0) { exit 1 }
        for (i = 0; i < ni; i++) print inc[i]
        print ""
        print "int main(void)"
        print "{"
        for (i = 0; i < nb; i++) print body[i]
        print "return 0;"
        print "}"
    }
' "$README" > "$SRC" || { echo "check-readme: no \`\`\`c block found in README.md"; exit 1; }

echo "README snippet: compile $SRC"
# shellcheck disable=SC2086 # CC and LDFLAGS are deliberately word-split
$CC -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -I"$DIR/../include" \
    -o "$BIN" "$SRC" "$STLIB" $LDFLAGS || { echo "check-readme: README snippet does not compile"; exit 1; }
echo "README snippet: run"
"$BIN" > "$BIN.out" || { cat "$BIN.out"; echo "check-readme: README snippet exited nonzero"; exit 1; }
cat "$BIN.out"
# The snippet's comment promises ids 1 and 2.
printf 'matched 1\nmatched 2\n' | cmp -s - "$BIN.out" \
    || { echo "check-readme: README snippet did not print 'matched 1' and 'matched 2'"; exit 1; }
