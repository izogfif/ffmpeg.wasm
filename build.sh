#!/bin/bash
set -euo pipefail
make prd
cp packages/core/dist/umd/* ../ogv.js/build/demo/lib/
