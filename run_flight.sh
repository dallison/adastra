#!/bin/bash

export RUNFILES_DIR=bazel-bin//flight/flight.runfiles

SILENT=true

echo Running stagezero
bazel-bin/flight/flight.runfiles/__main__/stagezero/stagezero --silent=$SILENT 0<&1 &
s0_pid=$!
sleep 1

echo Running capcom
bazel-bin/flight/flight.runfiles/__main__/capcom/capcom --silent=$SILENT 0<&1 &
capcom_pid=$!
sleep 1
trap "kill -INT $s0_pid $capcom_pid; exit" SIGINT EXIT

bazel-bin//flight/flight.runfiles/__main__/flight/flight_director $*

