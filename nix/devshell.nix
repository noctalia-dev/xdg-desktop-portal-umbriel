{
  pkgs,
  xdg-desktop-portal-umbriel,
}:
pkgs.mkShell {
  inputsFrom = [ xdg-desktop-portal-umbriel ];

  nativeBuildInputs = with pkgs; [
    just
    lefthook
    meson
    ninja
    pkg-config
    wayland-scanner
    llvmPackages_22.clang-tools
    llvmPackages_22.libclang
    gnugrep
    gnused
    findutils
    gdb
    # Supplies dbus-daemon and dbus-send for the private-bus integration check.
    dbus
  ];

  buildInputs = with pkgs; [
    wayland
    wayland-protocols
    sdbus-cpp_2
    systemd
    pipewire
    libdrm
    libgbm
    cairo
    tomlplusplus
    nlohmann_json
    gtk4
    # Builds the EI receiver used by InputCapture integration checks.
    libei
  ];

  shellHook = ''
    echo " xdg-desktop-portal-umbriel dev-shell | 'just --list' to see available tasks"
  '';
}
