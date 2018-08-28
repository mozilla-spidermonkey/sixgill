#!/bin/bash

# SRCDIR=$HOME/src/MI-GC MESSAGE="Test for automated sixgill uploading, delete at will" ./release.sh --disable-cvc3
# SRCDIR=$HOME/src/MI-GC/js/src MESSAGE="Test for b2g sixgill automation, delete at will" TARGET_CC=/builds/slave/testing/build/target_compiler/gcc/linux-x86/arm/arm-linux-androideabi-4.7/bin/arm-linux-androideabi-gcc ./release.sh

set -e

TT_UPLOAD_SERVER=tooltool-uploads.pub.build.mozilla.org
TT_UPLOAD_PATH=/tooltool/uploads/"$USER"/pvt
TOOLTOOL=~/src/build-tooltool/tooltool.py
MESSAGE=

DO_CLEAN=1
DO_BUILD=1
DO_TEST=
DO_PACKAGE=1
DO_EMPLACE=1
DO_DISTRIBUTE=1
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
    DO_EMPLACE=
    DO_DISTRIBUTE=
  elif [ "$1" = "--build" ]; then
    shift
    DO_BUILD=1
    DO_TEST=
    DO_PACKAGE=1
    DO_EMPLACE=
    DO_DISTRIBUTE=
  elif [ "$1" = "--folder" ]; then
    shift
    FOLDER="$1"
    DO_CLEAN=
    DO_BUILD=
    DO_PACKAGE=
    shift
  elif [ "$1" = "--no-distribute" ]; then
    shift
    DO_DISTRIBUTE=
  elif [ "$1" = "--distribute" ]; then
    shift
    DO_CLEAN=
    DO_BUILD=
    DO_PACKAGE=
    DO_EMPLACE=
  elif [ "$1" = "--srcdir" ]; then
    shift
    SRCDIR="$1"
    shift
  elif [ "$1" = "--message" ]; then
    shift
    MESSAGE="$1"
    shift
  elif [ "$1" = "--" ]; then
    shift
    break
  else
    break
  fi
done

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

function need_message() {
  if [ -z "$MESSAGE" ]; then
    echo -n "Message for tooltool upload> "
    read MESSAGE
  fi
}

function build() {
  ./autogen.sh
  ./configure "$@"
  make
}

function run_test() {
  echo '------------ running tests ---------------'
  [ -x "$PYTHON" ] || PYTHON=python2.7
  ( cd test; $PYTHON run-test.py )
}

function make_checksum() {
  FILE=${1:-$FOLDER/sixgill.tar.xz}
  CKSUM=$(sha512sum $FILE)
  CKSUM="${CKSUM% *}"
}

function package() {
  D=release-tmp
  set +e
  rm -rf $D
  mkdir -p $D/sixgill/{usr/bin,usr/libexec/sixgill/{gcc,scripts/wrap_gcc}}
  set -e
  cp bin/* $D/sixgill/usr/bin
  cp gcc/xgill.so $D/sixgill/usr/libexec/sixgill/gcc
  cp scripts/run* $D/sixgill/usr/libexec/sixgill/scripts
  cp scripts/wrap_gcc/* $D/sixgill/usr/libexec/sixgill/scripts/wrap_gcc
  rm $D/sixgill/usr/bin/*.a
  rm $D/sixgill/usr/bin/{xcheck,xinfer}
  [ -z "$NOSTRIP" ] && strip $D/sixgill/usr/bin/* || true
  ( cd $D && tar -Jcvf - sixgill ) > $D/sixgill.tar.xz
  make_checksum "$D/sixgill.tar.xz"
  SHORT=$(echo "$CKSUM" | cut -c-12)
  FOLDER="$SHORT-sixgill"
  mkdir $FOLDER
  mv $D/sixgill.tar.xz $FOLDER
  # cp setup.sh.sixgill $FOLDER
  echo "** Created folder $FOLDER"
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

function emplace() {
  need_srcdir
  (
    cd $FOLDER
    [ -f manifest.tt ] && rm manifest.tt
    for f in *; do [[ $f = manifest.tt ]] || python $TOOLTOOL add "$f" --visibility public --unpack; done
  )
  local digest=$(json $FOLDER/manifest.tt -e 'cat --nokeys 0/digest')
  local size=$(json $FOLDER/manifest.tt -e 'cat --nokeys 0/size')
  for manifest in $SRCDIR/browser/config/tooltool-manifests/linux64/hazard.manifest $SRCDIR/b2g/dev/config/tooltool-manifests/linux64/hazard.manifest $SRCDIR/js/src/devtools/rootAnalysis/build/sixgill.manifest; do
      json $manifest -e 'grep --cd sixgill.tar.xz */filename' -e "set digest=$digest" -e "set size=$size" -e "set hg_id \"$(hg id)\"" -e 'write --noindent'
  done
}

function distribute() {
  echo -n "Press enter to upload"
  read
  echo "Uploading to $TT_UPLOAD_PATH"
  ( cd $FOLDER && python $TOOLTOOL upload --message "$MESSAGE" --authentication-file=~/.ssh/tooltool-upload.tok )
  echo "Uploaded contents of $FOLDER"
}

if ! [[ -n "$DO_PACKAGE" ]]; then need_folder; fi
if [[ -n "$DO_EMPLACE" ]]; then need_srcdir; fi
if [[ -n "$DO_DISTRIBUTE" ]]; then need_message; fi

if [[ -n "$DO_BUILD" ]]; then build "$@"; fi
if [[ -n "$DO_TEST" ]]; then run_test; fi
if [[ -n "$DO_PACKAGE" ]]; then package; fi
if [[ -n "$DO_EMPLACE" ]]; then emplace; fi
if [[ -n "$DO_DISTRIBUTE" ]]; then distribute; fi
