{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  sdbus-cpp_2,
  systemd,
  pipewire,
  libdrm,
  libgbm,
  cairo,
  tomlplusplus,
  nlohmann_json,
  gtk4,
  libei,
}:
let
  inherit (builtins) head match readFile;
  version = head (match ".*\n  version: '([0-9][^']+)'.*" (readFile ../meson.build));
in
stdenv.mkDerivation {
  pname = "xdg-desktop-portal-umbriel";
  inherit version;

  src = lib.cleanSource ./..;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
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
  ];

  mesonBuildType = "release";
  doCheck = true;
  checkInputs = [ libei ];

  meta = with lib; {
    description = "xdg-desktop-portal backend for the Umbriel compositor";
    license = licenses.mit;
    platforms = platforms.linux;
  };
}
