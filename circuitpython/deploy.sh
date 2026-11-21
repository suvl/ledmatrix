#!/bin/sh
# Copy the project's code onto the board's CIRCUITPY drive.
set -eu
DRIVE=/Volumes/CIRCUITPY
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -d "$DRIVE" ] || { echo "CIRCUITPY not mounted" >&2; exit 1; }
cp "$HERE/code.py" "$DRIVE/code.py"
sync
echo "deployed code.py"
