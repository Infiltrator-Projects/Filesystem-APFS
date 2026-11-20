#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
module=${1:?APFS.so required}
test -s "$module"
test "$(nm -D --defined-only "$module" | awk '$3 == "infiltrator_filesystem_api_v2" { n++ } END { print n+0 }')" -eq 1
if nm -D --defined-only "$module" |
   grep -E 'infiltrator_filesystem_.*_v1($|@)|infiltrator_filesystem_module_v[0-9]+($|@)'; then
  echo 'An obsolete filesystem ABI is exported from APFS.so' >&2
  exit 1
fi
# The production dynamic table exports the one V2 entry point only.
unexpected="$(nm -D --defined-only "$module" |
   awk '{ print $3 }' | sed -E 's/@@?.*$//' |
   grep -v '^infiltrator_filesystem_api_v2$' || true)"
if test -n "$unexpected"; then
  printf 'Unexpected APFS production exports:\n%s\n' "$unexpected" >&2
  exit 1
fi
echo 'APFS hosted Common V2-only ELF exports: PASS'
