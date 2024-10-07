#!/bin/bash

# This is used by stagezero to set the value of ${runfiles_dir}
export RUNFILES_DIR=bazel-bin/run_flight.runfiles

SILENT=true
TEST_MODE=false

# Uncomment this to run stagezero as root.
#SUDO=sudo

echo Running stagezero
if [[ "$SUDO" != "" ]]; then
  (cd $RUNFILES_DIR ; $SUDO ./_main/stagezero/stagezero --silent=$SILENT 0<&1 &)
else
  $RUNFILES_DIR/_main/stagezero/stagezero --silent=$SILENT 0<&1 &
fi
s0_pid=$!
sleep 1

echo Running capcom
bazel-bin/flight/flight.runfiles/_main/capcom/capcom --silent=$SILENT --test_mode=$TEST_MODE 0<&1 &
capcom_pid=$!
sleep 1
trap "kill -INT $s0_pid $capcom_pid; exit" SIGINT EXIT

bazel-bin//flight/flight.runfiles/_main/flight/flight_director $*
#wait

