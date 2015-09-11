#!/bin/bash

# Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE.md file.

# Build steps
#  - Run servicec.
#  - Build fletch library generators for target platforms (here ia32 and arm).
#  - In the servicec java output directory build libfletch using ndk-build.
#  - Copy/link output files servicec to the jni and java directories.
#  - Generate a snapshot of your Dart program and add it to you resources dir.

PROJ=github
ANDROID_PROJ=GithubMock
DART_FILE=bin/github_mock_service.dart
IDL_FILE=lib/src/github_mock.idl

set -ue

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

FLETCH_DIR="$(cd "$DIR/../../.." && pwd)"
FLETCH_PKG_DIR="$FLETCH_DIR/package"

# TODO(zerny): Support other modes than Release in tools/android_build/jni/Android.mk
TARGET_MODE=Release
TARGET_DIR="$(cd "$DIR/.." && pwd)"
TARGET_BUILD_DIR="$TARGET_DIR"

# TODO(zerny): Create a project specific package directory.
TARGET_PKG_DIR=$FLETCH_PKG_DIR #"$TARGET_BUILD_DIR/packages"

SERVICE_GEN_DIR="$TARGET_PKG_DIR/service"

JAVA_DIR=$DIR/$ANDROID_PROJ/app/src/main/java/fletch
JNI_LIBS_DIR=$DIR/$ANDROID_PROJ/app/src/main/jniLibs

DART="$FLETCH_DIR/out/ReleaseIA32/dart"
SERVICEC="$DART $FLETCH_DIR/tools/servicec/bin/servicec.dart"
FLETCH="$FLETCH_DIR/out/ReleaseIA32/fletch"

set -x

# Compile dart service file.
if [[ $# -eq 0 ]] || [[ "$1" == "service" ]]; then
    rm -rf "$SERVICE_GEN_DIR"
    mkdir -p "$SERVICE_GEN_DIR"
    $SERVICEC --out "$SERVICE_GEN_DIR" "$TARGET_DIR/$IDL_FILE"

    # TODO(zerny): Change the servicec output directory structure to allow easy
    # referencing from Android Studio.
    mkdir -p $JAVA_DIR
    cp -R $SERVICE_GEN_DIR/java/fletch/*.java $JAVA_DIR/
fi

# Build the native interpreter src for arm and x86.
if [[ $# -eq 0 ]] || [[ "$1" == "fletch" ]]; then
    cd $FLETCH_DIR
    ninja
    ninja -C out/${TARGET_MODE}XARMAndroid fletch_vm_library_generator
    ninja -C out/${TARGET_MODE}IA32Android fletch_vm_library_generator
    mkdir -p out/${TARGET_MODE}XARMAndroid/obj/src/vm/fletch_vm.gen
    mkdir -p out/${TARGET_MODE}IA32Android/obj/src/vm/fletch_vm.gen
    out/${TARGET_MODE}XARMAndroid/fletch_vm_library_generator > \
	out/${TARGET_MODE}XARMAndroid/obj/src/vm/fletch_vm.gen/generated.S
    out/${TARGET_MODE}IA32Android/fletch_vm_library_generator > \
	out/${TARGET_MODE}IA32Android/obj/src/vm/fletch_vm.gen/generated.S

    cd $SERVICE_GEN_DIR/java
    NDK_MODULE_PATH=. ndk-build

    mkdir -p $JNI_LIBS_DIR
    cp -R libs/* $JNI_LIBS_DIR/
fi

if [[ $# -eq 0 ]] || [[ "$1" == "http" ]]; then
    DATA_DIR=$TARGET_DIR/lib/src/github_mock_data
    DATA_FILE=$TARGET_DIR/lib/src/github_mock.data
    echo "const Map<String, List<int>> resources = const <String, List<int>> {" > $DATA_FILE
    cd $DATA_DIR
    for f in `find . -type f -name *\\\\.data`; do
	key=`echo $f | cut -b 3- | cut -d . -f 1`
	echo "'$key': const <int>[" >> $DATA_FILE
	od -A n -t d1 $f |\
          sed 's/\([^ ]\) /\1,/g' |\
          sed 's/\([^ ]\)$/\1,/' >> $DATA_FILE
	echo "]," >> $DATA_FILE
    done
    echo "};" >> $DATA_FILE
fi

if [[ $# -eq 0 ]] || [[ "$1" == "snapshot" ]]; then
    cd $FLETCH_DIR
    ninja -C out/ReleaseIA32

    # Kill the persistent process
    ./tools/persistent_process_info.sh --kill

    SNAPSHOT="$DIR/$ANDROID_PROJ/app/src/main/res/raw/snapshot"
    mkdir -p `dirname "$SNAPSHOT"`
    $FLETCH compile-and-run -o "$SNAPSHOT" "$TARGET_BUILD_DIR/$DART_FILE"
fi

set +x

if [[ $# -eq 1 ]]; then
    echo
    echo "Only ran task $1."
    echo "Possible tasks: service, fletch, and snapshot"
    echo "If Fletch or any IMMI files changed re-run compile.sh without arguments."
fi
