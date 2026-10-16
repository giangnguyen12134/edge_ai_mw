#!/bin/bash
# One-stop build for either target.
#
#   tools/build.sh x64                 # Ubuntu x86-64, build/x64
#   tools/build.sh arm64 --sysroot S   # Genio 720 cross build, build/arm64
#   tools/build.sh x64 --test          # also run ctest
#   tools/build.sh arm64 --sysroot S --package
#
# Extra arguments after "--" go to cmake, e.g.  -- -DMW_ENABLE_ALSA=OFF
set -eu
cd "$(dirname "$0")/.."

ARCH="${1:-}"
case "$ARCH" in
  x64|arm64) shift ;;
  *) echo "usage: $0 x64|arm64 [--sysroot DIR] [--test] [--package] [--debug] [-- cmake args]"; exit 2 ;;
esac

SYSROOT=""; TEST=0; PACKAGE=0; TYPE=Release
while [ $# -gt 0 ]; do
  case "$1" in
    --sysroot) SYSROOT="$2"; shift 2 ;;
    --test) TEST=1; shift ;;
    --package) PACKAGE=1; shift ;;
    --debug) TYPE=Debug; shift ;;
    --) shift; break ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done

DIR="build/$ARCH"
ARGS=(-DMW_TARGET_ARCH="$ARCH" -DCMAKE_BUILD_TYPE="$TYPE")
[ -n "$SYSROOT" ] && ARGS+=(-DMW_SYSROOT="$SYSROOT")
cmake -S . -B "$DIR" "${ARGS[@]}" "$@"
cmake --build "$DIR" -- -j"$(nproc)"
if [ "$TEST" = 1 ]; then
  (cd "$DIR" && ctest --output-on-failure)
fi
if [ "$PACKAGE" = 1 ]; then
  (cd "$DIR" && cpack)
fi
echo "built: $DIR"
