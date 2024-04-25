#!/bin/bash
# Bash script
./build.sh
pushd ../build
./handmade -t square -l
popd
