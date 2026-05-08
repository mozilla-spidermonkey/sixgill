#!/bin/bash

# See `build` script in same directory for current sample usage.

set -e

DO_CLEAN=1
DO_BUILD=1
DO_TEST=
DO_PACKAGE=1

# The wasm component is built by default, using the Emscripten SDK found via
# $EMSDK. --with-emsdk DIR overrides the location; --without-emsdk skips wasm.
WITH_EMSDK=1
EMSDK_DIR="${EMSDK:-}"

while [ "$#" -gt 0 ]; do
  if [ "$1" = "--no-clean" ]; then
    shift
    DO_CLEAN=
  elif [ "$1" = "--no-build" ]; then
    shift
    DO_CLEAN=
    DO_BUILD=
  elif [ "$1" = "--test" ]; then
    shift
    DO_TEST=1
  elif [ "$1" = "--no-test" ]; then
    shift
    DO_TEST=
  elif [ "$1" = "--build-and-package" ]; then
    shift
    DO_BUILD=1
    DO_TEST=1
    DO_PACKAGE=1
  elif [ "$1" = "--build" ]; then
    shift
    DO_BUILD=1
    DO_TEST=
    DO_PACKAGE=1
  elif [ "$1" = "--folder" ]; then
    shift
    FOLDER="$1"
    DO_CLEAN=
    DO_BUILD=
    DO_PACKAGE=
    shift
  elif [ "$1" = "--srcdir" ]; then
    shift
    SRCDIR="$1"
    shift
  elif [ "$1" = "--target-cc" ]; then
    shift
    TARGET_CC="$1"
    export TARGET_CC
    if [ -d "$TARGET_CC/gcc" ]; then
        TARGET_CC="$TARGET_CC/gcc"
    fi
    if [ -d "$TARGET_CC/bin" ]; then
        TARGET_CC="$TARGET_CC/bin"
    fi
    if [ -f "$TARGET_CC/gcc" ]; then
        TARGET_CC="$TARGET_CC/gcc"
    fi
    shift
    echo "SET TARGET_CC TO $TARGET_CC"
  elif [ "$1" = "--with-emsdk" ]; then
    shift
    EMSDK_DIR="$1"
    WITH_EMSDK=1
    shift
  elif [[ "$1" == --with-emsdk=* ]]; then
    EMSDK_DIR="${1#*=}"
    WITH_EMSDK=1
    shift
  elif [ "$1" = "--without-emsdk" ]; then
    shift
    WITH_EMSDK=
  elif [ "$1" = "--" ]; then
    shift
    break
  else
    break
  fi
done

# Resolve the Emscripten SDK unless --without-emsdk was given. Fail loudly if it
# is required but cannot be found, rather than dying later inside make on emcc.
if [ -n "$WITH_EMSDK" ]; then
  emsdk_hint="set \$EMSDK or pass --with-emsdk DIR, or pass --without-emsdk to build without wasm"
  if [ -z "$EMSDK_DIR" ]; then
    echo "error: Emscripten SDK not specified: $emsdk_hint" >&2
    exit 1
  fi
  if [ ! -f "$EMSDK_DIR/emsdk_env.sh" ]; then
    echo "error: $EMSDK_DIR/emsdk_env.sh not found: $emsdk_hint" >&2
    exit 1
  fi
  EMSDK_QUIET=1 source "$EMSDK_DIR/emsdk_env.sh"
  export EM_CACHE="$(dirname "$0")/local/emscripten_cache"
  mkdir -p "$EM_CACHE"
else
  # Tell make to drop the wasm target and its install step.
  export WITHOUT_WASM=1
fi

# function need_compiler() {
#   COMPILER="${COMPILER:-$CC}"
#   if [ -z "$COMPILER" ]; then
#     COMPILER=$(which gcc 2>/dev/null)
#   fi
#   if [ -z "$COMPILER" ]; then
#     echo -n "COMPILER unset. Enter the path to the compiler that will build the plugin> "
#     read COMPILER
#   fi
# }

function build() {
  ./autogen.sh
  ./configure "$@"
  make
}

function run_test() {
  echo '------------ running tests ---------------'
  ( cd test; ${PYTHON:-python3} run-test.py )

  # The wasm tests need the node build of the module (emcc) and consume the
  # database produced by the suite above, so run them here when emsdk is enabled.
  if [ -n "$WITH_EMSDK" ]; then
    echo '------------ running wasm tests ---------------'
    make wasm-test
  fi
}

function make_checksum() {
  FILE=${1:-$FOLDER/sixgill.tar.xz}
  CKSUM=$(sha512sum $FILE)
  CKSUM="${CKSUM% *}"
}

function package() {
  rm -rf release-tmp || true
  DESTDIR=release-tmp/sixgill make install
}

function need_folder() {
  if [ -z "$FOLDER" ]; then
    echo -n "Enter folder> "
    read FOLDER
    make_checksum
  fi
}

function need_srcdir() {
  if [ -z "$SRCDIR" ]; then
    echo -n "Enter path to gecko checkout root> "
    read SRCDIR
  fi
  if [ ! -d "$SRCDIR" ]; then
    echo "Invalid srcdir" >&2
    exit 1
  fi
}

if ! [[ -n "$DO_PACKAGE" ]]; then need_folder; fi

if [[ -n "$DO_BUILD" ]]; then build "$@"; fi
if [[ -n "$DO_TEST" ]]; then run_test; fi
if [[ -n "$DO_PACKAGE" ]]; then package; fi
