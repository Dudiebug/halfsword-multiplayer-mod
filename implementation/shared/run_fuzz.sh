#!/bin/bash
# Fuzzes the wire-protocol decoders under ASan/UBSan. Exit 0 only if no sanitizer error and no
# round-trip mismatch occurs. --negative-control builds the harness with a known out-of-bounds decoder,
# which must make this gate fail (proves the gate can fail).
#   run_fuzz.sh [--negative-control] [ITERATIONS]
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
defs=()
if [ "${1:-}" = "--negative-control" ]; then defs=(-DHSM_FUZZ_NEGATIVE_CONTROL); shift; fi
iterations="${1:-300000}"
out="${TMPDIR:-/tmp}/hsm-fuzz-$$"; mkdir -p "$out"; trap 'rm -rf "$out"' EXIT

compile_cmd=(); run_prefix=()
for c in "${CXX:-}" g++ clang++ c++; do
  if [ -n "$c" ] && command -v "$c" >/dev/null 2>&1; then compile_cmd=("$c"); break; fi
done
if [ ${#compile_cmd[@]} -eq 0 ]; then
  if command -v flatpak >/dev/null 2>&1 && flatpak info org.freedesktop.Sdk//25.08 >/dev/null 2>&1; then
    run_prefix=(flatpak run --filesystem=home --filesystem="$out" --command=env org.freedesktop.Sdk//25.08)
    compile_cmd=("${run_prefix[@]}" g++)
  else
    echo "run_fuzz: no C++ compiler" >&2; exit 2
  fi
fi

"${compile_cmd[@]}" -std=c++20 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  "${defs[@]}" -I"$here/protocol" -I"$here" -o "$out/fuzz" "$here/protocol/fuzz_hsm_protocol.cpp" || { echo "run_fuzz: compile failed"; exit 1; }
"${run_prefix[@]}" env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 "$out/fuzz" "$iterations"
