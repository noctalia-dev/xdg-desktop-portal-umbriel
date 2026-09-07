# xdg-desktop-portal-umbriel

An [xdg-desktop-portal](https://github.com/flatpak/xdg-desktop-portal) backend for the [Umbriel](https://github.com/noctalia-dev/umbriel) compositor.

## Supported interfaces

- `org.freedesktop.impl.portal.ScreenCast`
- `org.freedesktop.impl.portal.Screenshot`

## Building

Requires Meson >= 1.3 and a C++23 compiler.

### Dependencies

- sdbus-c++ >= 2.0
- libpipewire-0.3
- wayland-client
- wayland-protocols >= 1.39
- libdrm
- gbm
- cairo
- tomlplusplus
- GTK4 (optional, for the share picker)

### With just

```sh
just configure
just build
```

Modes: `debug` (default), `release`, `asan`.

```sh
just build release
just install        # release build + sudo meson install
just uninstall      # remove files installed by just install
```

### Manual

```sh
meson setup build
meson compile -C build
sudo meson install -C build
```

### Nix

```sh
nix build
```

A dev shell is also available via `nix develop`.

## Share picker

The picker shows screen and window thumbnails in a responsive grid. Click a card
and choose **Share**; requests that allow multiple sources support clicking cards
to select or deselect them, including across tabs. Space toggles a focused card in
multiple-selection mode, Enter shares, and Escape cancels.

Window decorations are delegated to the compositor: the picker sets `GTK_CSD=0`
and draws no titlebar, so Umbriel owns the border and corner clipping. Controls
follow Umbriel's live palette and corner radius, with GTK theme colors as a
fallback. The picker defaults to Cairo rendering to avoid GPU renderer startup
and shutdown overhead; explicit `GTK_CSD` or `GSK_RENDERER` settings still take
precedence.

Screen and window previews are snapshots taken as cards become visible using the same Wayland image
capture protocols as screencasting. Hidden tabs and offscreen cards are deferred:
captures start when a card is scrolled or switched into view, and each finished
thumbnail is handed to the UI on the main loop. Each capture session is destroyed
and its cleanup acknowledged before the worker goes idle. Snapshots load
asynchronously, remain in memory,
and are discarded when the picker closes. Sources without a supported preview
remain selectable with a placeholder. The chooser's JSON input/output format is
unchanged. Window capture requires Umbriel's fix for output membership and frame
pacing across the desktop and capture scenes. Older compositor builds can leave
applications waiting for frame callbacks after a window capture ends.

## Configuration

The config file lives at `$XDG_CONFIG_HOME/xdg-desktop-portal-umbriel/config.toml` (or the system-installed default).

```toml
[screencast]
chooser_cmd = "/usr/local/libexec/umbriel-share-picker"
max_fps = 0

[screenshot]
cmd = ""
color_pick_cmd = ""
```

## Screencast cursor modes

Applications can request hidden, embedded, or metadata cursors for a screencast. Metadata cursors are published through PipeWire when version 1.4.8 or newer is available. Older PipeWire versions, or Wayland sessions without a pointer, use an embedded cursor instead.

## License

MIT License. See [LICENSE](LICENSE) for details.
