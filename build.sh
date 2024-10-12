#!/bin/bash
set -euo pipefail
make prd-mt
cp packages/core/dist/umd/* ../ogv.js/build/demo/lib/
