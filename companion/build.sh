#!/usr/bin/env bash
# build.sh — build companion_gui.py into a standalone executable
# Run from the companion/ directory.

set -e
echo "=== C7 VAG Companion — build ==="

pip install -r requirements.txt --quiet

PLATFORM=$(python3 -c "import sys; print(sys.platform)")
echo "Platform: $PLATFORM"

if [ "$PLATFORM" = "win32" ] || [ "$PLATFORM" = "win64" ]; then
    pyinstaller \
        --onefile \
        --windowed \
        --name "C7_VAG_Companion" \
        --add-data "ble_bridge_client.py;." \
        companion_gui.py
else
    pyinstaller \
        --onefile \
        --windowed \
        --name "C7_VAG_Companion" \
        --add-data "ble_bridge_client.py:." \
        companion_gui.py
fi

echo ""
echo "Output: dist/C7_VAG_Companion"
