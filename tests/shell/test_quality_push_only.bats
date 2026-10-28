#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The commit hook (quality-checks.sh --staged-only) runs only the fast gates.
# The slow ones are listed in QC_PUSH_ONLY and deferred to the pre-push and
# land-gate runs, which gate nothing; a commit waits for its slowest gate, so one
# slow gate left in costs every commit.

load helpers

setup() {
  cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# Loads QC_PUSH_ONLY, qc_trigger_re and qc_wanted from the real driver, then runs $2
# with STAGED_ONLY=$1.
with_driver() {
  bash -c '
    STAGED_ONLY="$1"
    QC_STAGED_ALL="src/foo.cpp"
    eval "$(grep "^QC_PUSH_ONLY=" scripts/quality-checks.sh)"
    eval "$(sed -n "/^qc_trigger_re() {/,/^}/p; /^QC_SKIPPED=/p; /^QC_DEFERRED=/p; /^qc_wanted() {/,/^}/p" scripts/quality-checks.sh)"
    eval "$2"
  ' _ "$1" "$2"
}

@test "a commit defers every push-only gate and names it" {
  run with_driver true 'for g in $QC_PUSH_ONLY; do qc_wanted "$g" && echo "RAN $g"; done; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "deferred: qc_python_tests" "$output"
  contains "qc_hidden_tests" "$output"
  contains "qc_decl_ui" "$output"
}

@test "HELIX_QC_ALL_AT_COMMIT=1 runs the push-only gates at commit" {
  run with_driver true 'export HELIX_QC_ALL_AT_COMMIT=1; qc_wanted qc_python_tests && echo RAN; echo "deferred:[$QC_DEFERRED]"'
  [ "$status" -eq 0 ]
  contains "RAN" "$output"
  contains "deferred:[]" "$output"
}

@test "a full run (pre-push, land-gate) defers nothing" {
  run with_driver false 'for g in $QC_PUSH_ONLY; do qc_wanted "$g" || echo "DEFERRED $g"; done; echo done'
  [ "$status" -eq 0 ]
  lacks "DEFERRED" "$output"
}

@test "a fast gate with no trigger still runs at commit" {
  run with_driver true 'qc_wanted qc_some_fast_gate && echo RAN'
  [ "$status" -eq 0 ]
  contains "RAN" "$output"
}

@test "every push-only gate is a gate the driver runs" {
  run bash -c '
    eval "$(grep "^QC_PUSH_ONLY=" scripts/quality-checks.sh)"
    eval "$(grep "^QC_ALL=" scripts/quality-checks.sh)"
    for g in $QC_PUSH_ONLY; do
      case " $QC_ALL " in *" $g "*) ;; *) echo "NOT IN QC_ALL: $g" ;; esac
    done; echo checked'
  [ "$status" -eq 0 ]
  lacks "NOT IN QC_ALL" "$output"
  contains "checked" "$output"
}

@test "a staged header is compile-checked through one source that includes it" {
  run bash -c '. scripts/qc/_lib.sh; qc_one_includer include/utils/network_validation.h'
  [ "$status" -eq 0 ]
  [[ "$output" == src/* || "$output" == tests/* ]] || fail "not a source under src/ or tests/: $output"
  run grep -E "#include[[:space:]]+[\"<]([^\">]*/)?network_validation\\.h[\">]" "$output"
  [ "$status" -eq 0 ]
}

@test "a header nothing includes yields no unit" {
  run bash -c '. scripts/qc/_lib.sh; qc_one_includer include/helix_no_such_header_xyz.h; echo "[end]"'
  [ "$status" -eq 0 ]
  [ "$output" = "[end]" ]
}
