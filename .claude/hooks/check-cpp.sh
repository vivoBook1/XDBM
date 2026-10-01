#!/bin/sh
# PostToolUse hook: syntax-check an edited .cpp/.h file. Errors and warnings
# are sent back to Claude (exit 2); anything else passes silently.
f=$(jq -r '.tool_input.file_path // empty')
case "$f" in
    *.cpp) extra= ;;
    # A header compiled on its own uses none of its constants or inline
    # functions, so those "unused" warnings are noise.
    *.h) extra="-Wno-pragma-once-outside-header -Wno-unused-const-variable -Wno-unused-function" ;;
    *) exit 0 ;;
esac
[ -f "$f" ] || exit 0

# cli/ and tests/ include engine headers from src/, as the Makefile's -Isrc does.
root=$(cd "$(dirname "$0")/../.." && pwd)

# $extra is intentionally unquoted so it splits into separate flags.
out=$(c++ -std=c++17 -Wall -Wextra -I"$root/src" $extra -fsyntax-only -x c++ "$f" 2>&1)
if [ $? -ne 0 ] || [ -n "$out" ]; then
    printf '%s\n' "$out" >&2
    exit 2
fi
exit 0
