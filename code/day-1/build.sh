#!/bin/bash
# Bash script
mkdir -p ../build
gcc xcb_handmade.c -o ../build/handmade -O0 -lxcb
