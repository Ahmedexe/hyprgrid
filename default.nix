{
  lib,
  hyprland,
  hyprlandPlugins,
}:
hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "hyprgrid-native";
  version = "0.7.1";
  src = ./.;

  inherit (hyprland) nativeBuildInputs;

  meta = {
    description = "Native cancellable 2D workspace-grid gestures for Hyprland";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
