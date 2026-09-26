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
remain selectable with a placeholder. Window capture requires Umbriel's fix for output membership and frame
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

Applications can request hidden, embedded, or metadata cursors for a screencast. Metadata cursors are published through PipeWire when version 1.4.8 or newer is available. If cursor metadata is unavailable, the cursor remains hidden so that a metadata request never burns it into the captured pixels.

## Changeable screencast targets

Every share with exactly one selected screen or window can be changed later with Umbriel actions. The selected source
starts sharing immediately. Shares containing several selected sources remain fixed because one target action cannot
unambiguously identify which stream to replace.

The first set or follow action while a changeable share is active opens a confirmation panel in Umbriel. Dismissing
the panel cancels that action and asks again the next time. After confirmation, later target changes are immediate
until the share ends. This protection can be disabled with Umbriel's
`screencast.disable_dynamic_confirmation` setting. Following lasts only for the active portal session. Window
following can expose any application you focus, so use it only when every window you may visit is safe to share.

Configure any of these Umbriel actions as keybinds or event actions:

- `screencast-set-window` selects the focused window and switches to manual mode.
- `screencast-set-window:<window-id>` selects a window by its IPC identifier and switches to manual mode.
- `screencast-set-output` selects the focused output and switches to manual mode.
- `screencast-set-output:<output>` selects a named output and switches to manual mode.
- `screencast-follow-window` follows the focused window.
- `screencast-follow-output` follows the focused output.
- `screencast-follow-stop` stops following while keeping the current target.
- `screencast-clear` stops following and pauses the stream.

Selecting another target replaces only the Wayland capture source. The PipeWire node remains the same, so
applications keep the original share without opening another chooser. Window and output targets are interchangeable
on that stream. Applications can restore the selected source without reopening the chooser, and a restored
single-source stream remains changeable. Immediate follow-up sessions from the same application reuse the mode and
target from the completed request,
including when that short-lived request closes during handoff. The grant expires after two seconds and is invalidated
when the target changes or clears. Another application or an unrelated later share still requires a fresh picker
choice.

## License

MIT License. See [LICENSE](LICENSE) for details.
