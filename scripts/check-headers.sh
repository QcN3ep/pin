#!/usr/bin/env bash
# Compiles every public header standalone to catch missing includes.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-c++}"
std="${PIN_CXX_STANDARD:-c++23}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

status=0
checked=0

for header in "$root"/include/pin/*.hpp; do
    name="$(basename "$header")"
    printf '#include <pin/%s>\nint main() { return 0; }\n' "$name" > "$tmp/check.cpp"

    if "$cxx" -std="$std" -I"$root/include" \
        -Wall -Wextra -Wpedantic -Werror -fsyntax-only "$tmp/check.cpp"; then
        checked=$((checked + 1))
    else
        echo "FAIL: <pin/$name> is not self-contained with $cxx" >&2
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "OK: $checked headers are self-contained ($cxx, -std=$std)"
fi

exit "$status"
