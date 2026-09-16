#!/usr/bin/env bash
# Verify GXRuntime's CPUState is byte-identical to DolRecomp's.
#
# The LLVM backend bakes offsetof() values taken from DolRecomp's cpu.h directly
# into generated code, while the C backend emits field names compiled against
# GXRuntime's cpu.h. So a field added to one struct and not the other breaks
# *only* LLVM modules, and breaks them as a segfault with no diagnostic: the
# generated code loads a neighbouring field as the MEM2 base pointer. That is
# what a missing `cycle_budget` did, and nothing in either build caught it.
#
# Run after any change to either header, and after every vendored-DolRecomp bump.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC=${CC:-"C:/Program Files/LLVM/bin/clang.exe"}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Fields whose offsets the LLVM backend emits, plus the overall size. Extend this
# list rather than trusting that a new field landed in both headers.
FIELDS="gpr fpr ps1 pc lr ctr cr xer fpscr msr external_addr external_value \
external_rid host_call external_user_data ram ram_size external_pointer \
downcount cycle_budget exram exram_size"

gen () {
  { echo "#include \"$1\""
    echo '#include <stddef.h>'
    echo '#include <stdio.h>'
    echo 'int main(void){'
    echo '  printf("sizeof %zu\n", sizeof(CPUState));'
    for f in $FIELDS; do
      echo "  printf(\"$f %zu\n\", offsetof(CPUState, $f));"
    done
    echo '  return 0; }'
  } > "$2"
}

gen "core/cpu.h"  "$TMP/gx.c"
gen "cpu/cpu.h"   "$TMP/dr.c"

"$CC" -I "$ROOT/lib/ModernGekko/vendor/dolphin/GXRuntime/include" "$TMP/gx.c" -o "$TMP/gx.exe" || {
  echo "FAIL: GXRuntime header does not compile (a field in the list above may be missing)" >&2; exit 1; }
"$CC" -I "$ROOT/lib/ModernGekko/vendor/dolphin/DolRecomp/src"     "$TMP/dr.c" -o "$TMP/dr.exe" || {
  echo "FAIL: DolRecomp header does not compile" >&2; exit 1; }

"$TMP/gx.exe" > "$TMP/gx.txt"
"$TMP/dr.exe" > "$TMP/dr.txt"

if diff -u "$TMP/dr.txt" "$TMP/gx.txt" > "$TMP/diff.txt"; then
  echo "CPU ABI OK: GXRuntime and DolRecomp CPUState layouts are identical"
  exit 0
fi
echo "FAIL: CPUState layout mismatch (-DolRecomp +GXRuntime)" >&2
cat "$TMP/diff.txt" >&2
echo >&2
echo "LLVM-backend modules will segfault until these agree. Fix the header that" >&2
echo "is behind and bump MODERNGEKKO_CPU_ABI_VERSION so stale modules cannot be" >&2
echo "served from the module cache." >&2
exit 1
