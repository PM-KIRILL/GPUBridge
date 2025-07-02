#!/bin/bash
set -e
cd codegen
python3 ./codegen.py
cd ..
cmake .
cmake --build .