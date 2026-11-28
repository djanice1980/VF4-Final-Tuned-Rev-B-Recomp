#!/bin/sh
# Creates a desktop entry for a native Linux release of VF4 Final Tuned.
# Run it from the extracted release folder (or its builder/tools folder).
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAUNCHER="$ROOT/vf4ft-launcher"
if [ ! -x "$LAUNCHER" ]; then
    echo "vf4ft-launcher not found or not executable in $ROOT" >&2
    exit 1
fi

ICON=""
for cand in "$ROOT/steam-art/icon.png" "$ROOT/builder/runtime/ui/kofi_badge.png"; do
    if [ -f "$cand" ]; then
        ICON="$cand"
        break
    fi
done

APPDIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPDIR"
DESKTOP="$APPDIR/vf4ft-launcher.desktop"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Virtua Fighter 4 Final Tuned
Comment=VF4 Final Tuned recomp (native Linux)
Exec=$LAUNCHER
Path=$ROOT
Terminal=false
Categories=Game;
EOF
if [ -n "$ICON" ]; then
    printf 'Icon=%s\n' "$ICON" >> "$DESKTOP"
fi
chmod +x "$DESKTOP"
echo "wrote $DESKTOP"
echo "you may need to log out and back in, or run: update-desktop-database $APPDIR"