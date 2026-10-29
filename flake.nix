{
  description = "Native cancellable 2D workspace-grid gestures for Hyprland 0.56.2";

  inputs = {
    hyprland.url = "github:hyprwm/Hyprland/efb50993780079460b0cbed1363e2166a2de1d9f";
    nixpkgs.follows = "hyprland/nixpkgs";
    systems.follows = "hyprland/systems";
  };

  outputs = {
    self,
    hyprland,
    nixpkgs,
    systems,
    ...
  }: let
    inherit (nixpkgs) lib;
    eachSystem = lib.genAttrs (import systems);
    pkgsFor = eachSystem (system:
      import nixpkgs {
        localSystem.system = system;
        overlays = [
          self.overlays.default
          hyprland.overlays.hyprland-packages
        ];
      });
  in {
    overlays.default = final: prev: {
      hyprlandPlugins = (prev.hyprlandPlugins or {}) // {
        hyprgrid-native = final.callPackage ./default.nix {};
      };
    };

    packages = eachSystem (system: {
      default = pkgsFor.${system}.hyprlandPlugins.hyprgrid-native;
      hyprgrid-native = self.packages.${system}.default;
    });

    checks = eachSystem (system: self.packages.${system});
  };
}
