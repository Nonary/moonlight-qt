#!/usr/bin/env bash
set -euo pipefail
source_root=$(pwd)
qmake_command=${1:-qmake6}
jobs=${BUILD_JOBS:-2}
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
run_suite() {
    local project=$1 target=$2
    local directory="$source_root/build/integration-$target"
    mkdir -p "$directory"
    (
        cd "$directory"
        "$qmake_command" "$source_root/tests/$project.pro" CONFIG+=release
        make -j"$jobs"
        if [[ "$target" == tst_overlay ]]; then
            "./$target" "$source_root/app"
        else
            "./$target"
        fi
    )
}
run_suite vrr/preferences tst_vrrpreferences
run_suite vrr/timingcontroller tst_vrrtimingcontroller
run_suite vrr/pacingworker tst_vrrpacingworker
run_suite vrr/vrrcacheidentity tst_vrrcacheidentity
run_suite vrr/phasetrajectory tst_phasetrajectory
run_suite vrr/inputgoalintegration tst_inputgoalintegration
run_suite vrr/performancestats tst_performancestats
run_suite vrr/overlay tst_overlay
run_suite input/tolerance tst_toleranceinput
run_suite qml/calibration-hosts tst_calibrationhosts
run_suite qml/controller-navigation tst_controllernavigation
run_suite pyrowave/udpaddress tst_pyrowaveudpaddress
run_suite pyrowave/d3d11interop tst_pyrowaved3d11interop
