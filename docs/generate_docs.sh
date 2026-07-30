#!/usr/bin/env bash
set -euo pipefail

# Resolve paths relative to this script's location, so it works no
# matter where it's invoked from (project/, docs/, elsewhere).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export srctree="$SCRIPT_DIR/kerneldoc-src"

cd "$SCRIPT_DIR"

if [[ ! -d ".venv-docs" ]]; then
    uv venv .venv-docs
fi;
source .venv-docs/bin/activate
uv pip install "sphinx>=6"

python3 generate_rst.py
sphinx-build -b html sphinx _build/html

echo "Docs built: $SCRIPT_DIR/_build/html/index.html"