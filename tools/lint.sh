#!/bin/bash
# Style gate: clang-format (Google style) and cpplint. Run from anywhere.
#   tools/lint.sh          check only
#   tools/lint.sh --fix    reformat in place
set -eu
cd "$(dirname "$0")/.."
FILES=$(find include src apps tests -name '*.h' -o -name '*.cc')
if [ "${1:-}" = "--fix" ]; then
  echo "$FILES" | xargs clang-format -i
  exit 0
fi
echo "$FILES" | xargs clang-format --dry-run --Werror
cpplint --quiet $FILES
echo "style OK"
