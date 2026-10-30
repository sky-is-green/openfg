#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Install the openfg control panel for the current user.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
datadir="${XDG_DATA_HOME:-$HOME/.local/share}"
appdir="$datadir/openfg/gui"
bindir="$HOME/.local/bin"
icondir="$datadir/icons/hicolor/scalable/apps"
appl="$datadir/applications"

if ! python3 -c 'import PySide6' >/dev/null 2>&1; then
    echo "error: PySide6 is required (CachyOS: sudo pacman -S pyside6)" >&2
    exit 1
fi

mkdir -p "$appdir" "$bindir" "$icondir" "$appl"
install -m 0755 "$repo/gui/openfg-gui.py" "$appdir/openfg-gui.py"
install -m 0644 "$repo/gui/openfg.svg" "$icondir/openfg.svg"

cat > "$bindir/openfg-gui" <<EOF
#!/usr/bin/env bash
exec python3 "$appdir/openfg-gui.py" "\$@"
EOF
chmod 0755 "$bindir/openfg-gui"

cat > "$appl/openfg-gui.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=openfg Control
GenericName=Frame Generation Control
Comment=Configure openfg frame generation (GPU, multiplier, processes)
Exec=python3 $appdir/openfg-gui.py
Icon=openfg
Terminal=false
Categories=Settings;
Keywords=openfg;frame;generation;gpu;vulkan;
StartupNotify=true
StartupWMClass=openfg-gui
EOF

command -v update-desktop-database >/dev/null && update-desktop-database "$appl" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q "$datadir/icons/hicolor" 2>/dev/null || true
for k in kbuildsycoca6 kbuildsycoca5; do
    command -v "$k" >/dev/null && "$k" --noincremental >/dev/null 2>&1 && break
done

desktop_dir="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
[ -n "${desktop_dir:-}" ] || desktop_dir="$HOME/Desktop"
if [ -d "$desktop_dir" ]; then
    install -m 0755 "$appl/openfg-gui.desktop" "$desktop_dir/openfg-gui.desktop"
    echo "Installed desktop shortcut: $desktop_dir/openfg-gui.desktop"
fi

echo
echo "Installed GUI:"
echo "  launcher : $bindir/openfg-gui"
echo "  desktop  : $appl/openfg-gui.desktop"
echo "  icon     : $icondir/openfg.svg"
echo
echo "Run 'openfg-gui' or find \"openfg Control\" in the application menu."
