#!/bin/bash

# MANIFEST=$HOME/src/MI-GC MESSAGE="Test for automated sixgill uploading, delete at will" ./release.sh --disable-cvc3
# MANIFEST=$HOME/src/MI-GC/js/src/devtools/rootAnalysis/build/sixgill-b2g.manifest MESSAGE="Test for b2g sixgill automation, delete at will" TARGET_CC=/builds/slave/testing/build/target_compiler/gcc/linux-x86/arm/arm-linux-androideabi-4.7/bin/arm-linux-androideabi-gcc ./release.sh

set -e

TT_UPLOAD_SERVER=tooltool-uploads.pub.build.mozilla.org
TT_UPLOAD_PATH=/tooltool/uploads/"$USER"/pvt
TOOLTOOL=~/src/build-tooltool/tooltool.py
MESSAGE=

DO_CLEAN=1
DO_BUILD=1
DO_PACKAGE=1
DO_EMPLACE=1
DO_DISTRIBUTE=1
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--noclean" ]; then
    shift
    DO_CLEAN=
  elif [ "$1" = "--nobuild" ]; then
    shift
    DO_CLEAN=
    DO_BUILD=
  elif [ "$1" = "--build-and-package" ]; then
    shift
    DO_BUILD=1
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
  elif [ "$1" = "--distribute" ]; then
    shift
    DO_CLEAN=
    DO_BUILD=
    DO_PACKAGE=
    DO_EMPLACE=
  elif [ "$1" = "--manifest" ]; then
    shift
    MANIFEST="$1"
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

function need_target_cc() {
  if [ -z "$TARGET_CC" ]; then
   echo -n "TARGET_CC unset. Enter the path to the compiler that will run the plugin> "
    read TARGET_CC
  fi
  if [ ! -x "$TARGET_CC" ]; then
    echo "Invalid TARGET_CC: $TARGET_CC" >&2
    exit 1
  fi
}

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
  export TARGET_CC
  ./configure "$@"
  make
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
  strip $D/sixgill/usr/bin/* || true
  ( cd $D && tar -Jcvf - sixgill ) > $D/sixgill.tar.xz
  make_checksum "$D/sixgill.tar.xz"
  SHORT=$(echo "$CKSUM" | cut -c-12)
  FOLDER="$SHORT-sixgill"
  mkdir $FOLDER
  mv $D/sixgill.tar.xz $FOLDER
  cp setup.sh.sixgill $FOLDER
  echo "** Created folder $FOLDER"
}

function need_folder() {
  if [ -z "$FOLDER" ]; then
    echo -n "Enter folder> "
    read FOLDER
    make_checksum
  fi
}

function need_manifest() {
  if [ -z "$MANIFEST" ]; then
    echo -n "Enter manifest path (or path to gecko checkout root) "
    read MANIFEST
  fi
  if [ ! -f "$MANIFEST" ]; then
    MANIFEST="$MANIFEST/js/src/devtools/rootAnalysis/build/sixgill.manifest"
  fi
  if [ ! -f "$MANIFEST" ]; then
    echo "Invalid manifest" >&2
    exit 1
  fi
}

function emplace() {
  need_manifest
  (
    cd $FOLDER
    [ -f manifest.tt ] && rm manifest.tt
    for f in *; do [[ $f = manifest.tt ]] || python $TOOLTOOL add "$f" --visibility public; done
  )
  json $FOLDER/manifest.tt -e 'unshift {}' -e 'cd 0' -e "set hg_id \"$(hg id)\"" -e 'write /tmp/release.manifest.tmp'
  cp /tmp/release.manifest.tmp "$MANIFEST"
  ( cd $(dirname $MANIFEST) && hg diff $(basename $MANIFEST) )
}

function distribute() {
  echo -n "Press enter to upload"
  read
  echo "Uploading to $TT_UPLOAD_PATH"
  ( cd $FOLDER && python $TOOLTOOL upload --message "$MESSAGE" --authentication-file=~/.ssh/tooltool-upload.tok )
  echo "Uploaded contents of $FOLDER"
}

[ -n "$DO_BUILD" ] && need_target_cc
[ -n "$DO_PACKAGE" ] || need_folder
[ -n "$DO_EMPLACE" ] && need_manifest
[ -n "$DO_DISTRIBUTE" ] && need_message

[ -n "$DO_BUILD" ] && build "$@"
[ -n "$DO_PACKAGE" ] && package
[ -n "$DO_EMPLACE" ] && emplace
[ -n "$DO_DISTRIBUTE" ] && distribute
