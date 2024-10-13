#!/bin/bash
set -euo pipefail
# To build single-thread:
make prd
# To build multi-thread:
# make prd-mt
cp packages/core/dist/umd/* ../ogv.js/build/demo/lib/
