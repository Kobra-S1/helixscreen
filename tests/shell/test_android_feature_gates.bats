#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Android build compiles through Gradle/CMake and never reads the Makefile,
# so a `HELIX_HAS_X ?= 1` default there does not reach it. An undefined macro in
# `#if` is 0, which compiles the feature out with no error: a printer's filament
# system simply never appears in the app. Every Makefile feature gate therefore
# needs an explicit value in the Android CMake.

CMAKE="android/app/jni/CMakeLists.txt"

@test "every Makefile feature gate has an explicit value in the Android CMake" {
  gates=$(grep -oE '^HELIX_HAS_[A-Z0-9_]+ \?=' Makefile | cut -d' ' -f1 | sort -u)
  [ -n "$gates" ]
  missing=""
  for g in $gates; do
    grep -qE "^[[:space:]]+${g}=[01]\b" "$CMAKE" || missing="$missing $g"
  done
  if [ -n "$missing" ]; then
    echo "Missing from $CMAKE:$missing"
    return 1
  fi
}

@test "the Android app keeps every network-reachable vendor filament system" {
  for g in HELIX_HAS_CFS HELIX_HAS_IFS HELIX_HAS_ACE HELIX_HAS_QIDI HELIX_HAS_SNAPMAKER; do
    grep -qE "^[[:space:]]+${g}=1\b" "$CMAKE"
  done
}
