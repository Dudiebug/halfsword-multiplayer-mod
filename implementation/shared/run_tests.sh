#!/bin/bash
# Builds and runs every implementation/shared/**/test_*.cpp with ASan+UBSan and -Werror.
# Exit 0 only if every test binary compiles and exits 0. Uses $CXX, a host g++/clang++, or the
# org.freedesktop.Sdk flatpak's g++ when the host has no C++ compiler.
#   run_tests.sh            run all tests
#   run_tests.sh FILTER     run tests whose path contains FILTER
#   run_tests.sh --root DIR run test_*.cpp under DIR instead (used for the known-bad calibration fixture)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
search="$here"
if [ "${1:-}" = "--root" ]; then search="$(cd "$2" && pwd)"; shift 2; fi
filter="${1:-}"
out="${TMPDIR:-/tmp}/hsm-tests-$$"; mkdir -p "$out"; trap 'rm -rf "$out"' EXIT

compile_cmd=()
for c in "${CXX:-}" g++ clang++ c++; do
  if [ -n "$c" ] && command -v "$c" >/dev/null 2>&1; then compile_cmd=("$c"); break; fi
done
run_prefix=()
if [ ${#compile_cmd[@]} -eq 0 ]; then
  if command -v flatpak >/dev/null 2>&1 && flatpak info org.freedesktop.Sdk//25.08 >/dev/null 2>&1; then
    run_prefix=(flatpak run --filesystem=home --filesystem="$out" --command=env org.freedesktop.Sdk//25.08)
    compile_cmd=("${run_prefix[@]}" g++)
  else
    echo "run_tests: no C++ compiler (set CXX or install the flatpak org.freedesktop.Sdk//25.08)" >&2
    exit 2
  fi
fi

flags=(-std=c++20 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -I"$here/protocol" -I"$here")
pass=0; fail=0; ran=0
while IFS= read -r src; do
  case "$src" in *"$filter"*) ;; *) continue ;; esac
  name="$(basename "${src%.cpp}")"
  ran=$((ran + 1))
  if ! "${compile_cmd[@]}" "${flags[@]}" -o "$out/$name" "$src"; then
    echo "FAIL (compile) $name"; fail=$((fail + 1)); continue
  fi
  if "${run_prefix[@]}" env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 "$out/$name"; then
    echo "PASS $name"; pass=$((pass + 1))
  else
    echo "FAIL $name"; fail=$((fail + 1))
  fi
done < <(find "$search" -name 'test_*.cpp' | sort)

echo "run_tests: $ran test programs, $pass passed, $fail failed"
[ "$ran" -gt 0 ] && [ "$fail" -eq 0 ]
