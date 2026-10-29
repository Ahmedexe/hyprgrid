#!/usr/bin/env bash
# Build hyprgrid-native against the Hyprland installed by pacman and load it.
# Run it again after every Hyprland update (once you have logged back in).
set -euo pipefail
cd "$(dirname "$0")"

die() { echo "error: $*" >&2; exit 1; }

for cmd in g++ make pkg-config hyprctl; do
    command -v "$cmd" >/dev/null || die "'$cmd' not found. Install the build tools: sudo pacman -S --needed base-devel pkgconf"
done
pkg-config --exists hyprland || die "Hyprland headers not found (pkg-config hyprland). Install/repair the package: sudo pacman -S hyprland"
[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || die "run this from a terminal inside your Hyprland session"

installed=$(pkg-config --modversion hyprland)
running=$(hyprctl version -j | sed -n 's/.*"tag": *"v\{0,1\}\([^"-]*\).*/\1/p' | head -n1)
echo "Hyprland headers: $installed, running Hyprland: ${running:-unknown}"
if [ -n "$running" ] && [ "$running" != "$installed" ]; then
    die "the running Hyprland ($running) differs from the installed one ($installed). Log out and back in after the update, then run this again."
fi

make clean >/dev/null
make

dest="${XDG_DATA_HOME:-$HOME/.local/share}/hyprgrid-native"
so="$dest/hyprgrid-native.so"
mkdir -p "$dest"
hyprctl plugin unload "$so" >/dev/null 2>&1 || true
install -m 755 hyprgrid-native.so "$so"
hyprctl plugin load "$so"

bindir="$HOME/.local/bin"
mkdir -p "$bindir"
install -m 755 hyprgrid-goto "$bindir/hyprgrid-goto"

has_menu=no
for m in fuzzel wofi rofi tofi bemenu; do command -v "$m" >/dev/null && has_menu=yes; done

echo
echo "Loaded $so"
echo "Installed $bindir/hyprgrid-goto (the SUPER+G 'type two numbers' prompt)"
[ "$has_menu" = yes ] || echo "  note: it needs a menu tool: sudo pacman -S fuzzel"
echo
echo "Add this to your Hyprland config (change keys you already use):"
case "$installed" in
    0.5[0-6].*)
        cat <<EOF

  # hyprland.conf
  plugin = $so
  bind = SUPER, G, exec, $bindir/hyprgrid-goto
  bind = SUPER SHIFT, G, exec, $bindir/hyprgrid-goto take
  bind = SUPER, X, exec, hyprctl hyprgrid goto 0,0
EOF
        ;;
    *)
        cat <<EOF

  -- hyprland.lua
  hl.plugin.load("$so")
  hl.bind("SUPER + G", hl.dsp.exec_cmd("$bindir/hyprgrid-goto"))
  hl.bind("SUPER + SHIFT + G", hl.dsp.exec_cmd("$bindir/hyprgrid-goto take"))
  hl.bind("SUPER + X", hl.dsp.exec_cmd("hyprctl hyprgrid goto 0,0"))
EOF
        ;;
esac
