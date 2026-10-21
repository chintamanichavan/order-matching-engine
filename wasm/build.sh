#!/bin/sh
# Builds the browser demo's engine (rtrade.js + rtrade.wasm) into wasm/,
# using the Emscripten Docker image so nothing needs installing.
# Copy both files to the portfolio's public/ folder afterwards.
set -e
cd "$(dirname "$0")/.."
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -w /src emscripten/emsdk:4.0.10 \
  em++ -std=c++23 -O3 -Iinclude src/lob.cpp src/simulator.cpp wasm/bindings.cpp -lembind \
  -sMODULARIZE=1 -sEXPORT_NAME=createRtrade -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 \
  -o wasm/rtrade.js
