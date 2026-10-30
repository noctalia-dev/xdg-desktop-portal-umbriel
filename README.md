# xdg-desktop-portal-umbriel

An [xdg-desktop-portal](https://github.com/flatpak/xdg-desktop-portal) backend for the [Umbriel](https://github.com/noctalia-dev/umbriel) compositor.

## Supported interfaces

- `org.freedesktop.impl.portal.InputCapture` (version 2)
- `org.freedesktop.impl.portal.ScreenCast`
- `org.freedesktop.impl.portal.Screenshot`

## Input capture

InputCapture version 2 supports keyboard and pointer capture for input-sharing applications. The application creates a session with `CreateSession2()`, requests permission with `Start()`, connects to EIS, obtains logical output zones, sets screen-edge barriers, and enables the session. Crossing an accepted barrier starts capture. Umbriel sends captured input directly to the application through EI while suppressing local delivery of the captured capabilities.

| Feature                                         | Support                                                                     |
| ----------------------------------------------- | --------------------------------------------------------------------------- |
| Keyboard                                        | Supported, capability `1`, including keymaps and modifiers                  |
| Pointer                                         | Supported, capability `2`, including motion, buttons, and scrolling         |
| Keyboard and pointer together                   | Supported, capability mask `3`                                              |
| Logical output zones and exterior edge barriers | Supported, including negative coordinates and mixed output scales           |
| Permission approval                             | Required unless a valid remembered grant is restored                        |
| Permission persistence and restore tokens       | Supported, with explicit consent for application-lifetime or durable grants |
| Clipboard sharing                               | Unsupported                                                                 |
| Touchscreen, tablet, and gesture capture        | Unsupported                                                                 |
| RemoteDesktop input injection                   | Unsupported by this portal                                                  |

Permission is enforced by Umbriel. Press Enter on a physical keyboard to allow one capture session. When the application requests persistence, physical **R** remembers approval for the lifetime shown: while the application runs, or until revoked. Another non-modifier key or a pointer click denies it. The displayed application label is a claim rather than a verified identity. Restore tokens work for any compatible application, including empty application identifiers; names alone never restore access. Press **Ctrl+Alt+Escape** on a physical keyboard to stop active capture and restore local control. Session close, application or portal failure, locking, and session deactivation also stop capture.

A matching Umbriel build providing version 2 of `umbriel_input_capture_manager_v1` is required. This private bridge version is independent of the public InputCapture API version. The backend accepts control calls only from the desktop portal frontend's current D-Bus owner; applications should use `org.freedesktop.portal.InputCapture` through `xdg-desktop-portal`.

The application must request persistence and retain its portal restore token. Expired, revoked, malformed, or expanded grants require fresh physical approval. Clipboard sharing remains unsupported. Manage remembered grants with `umbriel input-capture-permissions` and `umbriel input-capture-revoke <id>` or `umbriel input-capture-revoke all`. Durable credentials remain in the compositor's private state store; permission listings do not expose them.

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
- libei >= 1.3 (pkg-config `libei-1.0`, only when tests are enabled, for the input-capture receiver helper)

The portal daemon forwards Umbriel's EIS connection FD and does not link libei itself. Pass `-Dtests=false` to Meson to omit test helpers and the test-only libei dependency. The Nix package supplies libei through its check inputs, and the development shell includes it for integration checks.

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

The picker shows screen and window thumbnails in a responsive grid. Click a card and choose **Share**; requests that allow multiple sources support clicking cards to select or deselect them, including across tabs. Single-source requests open with Umbriel's focused screen selected (or the only screen, when focus is unknown), so Enter shares it. Space toggles a focused card in multiple-selection mode, Enter shares, and Escape cancels.

Window decorations are delegated to the compositor: the picker sets `GTK_CSD=0` and draws no titlebar, so Umbriel owns the border and corner clipping. Controls follow Umbriel's live palette and corner radius, with GTK theme colors as a fallback. The picker defaults to Cairo rendering to avoid GPU renderer startup and shutdown overhead; explicit `GTK_CSD` or `GSK_RENDERER` settings still take precedence.

Screen and window previews are snapshots taken as cards become visible using the same Wayland image capture protocols as screencasting. Hidden tabs and offscreen cards are deferred: captures start when a card is scrolled or switched into view, and each finished thumbnail is handed to the UI on the main loop. Each capture session is destroyed and its cleanup acknowledged before the worker goes idle. Snapshots load asynchronously, remain in memory, and are discarded when the picker closes. Sources without a supported preview remain selectable with a placeholder. Window capture requires Umbriel's fix for output membership and frame pacing across the desktop and capture scenes. Older compositor builds can leave applications waiting for frame callbacks after a window capture ends.

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

Every share with exactly one selected screen or window can be changed later with Umbriel actions. The selected source starts sharing immediately. Shares containing several selected sources remain fixed because one target action cannot unambiguously identify which stream to replace.

The first set or follow action while a changeable share is active opens a confirmation panel in Umbriel. Dismissing the panel cancels that action and asks again the next time. After confirmation, later target changes are immediate until the share ends. This protection can be disabled with Umbriel's `screencast.disable_dynamic_confirmation` setting. Following lasts only for the active portal session. Window following can expose any application you focus, so use it only when every window you may visit is safe to share.

Configure any of these Umbriel actions as keybinds or event actions:

- `screencast-set-window` selects the focused window and switches to manual mode.
- `screencast-set-window <window-id>` selects a window by its IPC identifier and switches to manual mode.
- `screencast-set-output` selects the focused output and switches to manual mode.
- `screencast-set-output <output>` selects a named output and switches to manual mode.
- `screencast-follow-window` follows the focused window.
- `screencast-follow-output` follows the focused output.
- `screencast-follow-stop` stops following while keeping the current target.
- `screencast-clear` stops following and pauses the stream.

Selecting another target replaces only the Wayland capture source. The PipeWire node remains the same, so applications keep the original share without opening another chooser. Window and output targets are interchangeable on that stream. Applications can restore the selected source without reopening the chooser, and a restored single-source stream remains changeable. Immediate follow-up sessions from the same application reuse the mode and target from the completed request, including when that short-lived request closes during handoff. The grant expires after two seconds and is invalidated when the target changes or clears. Another application or an unrelated later share still requires a fresh picker choice.

## InputCapture verification

The receiver helper is built when Meson's `tests` option is enabled. The complete integration check lives in the Umbriel repository and uses a private D-Bus session, an isolated headless compositor, and backend-boundary input devices. It requires `dbus-daemon` and `dbus-send` from the `dbus` package, plus an `xdg-desktop-portal` frontend executable; the frontend path is supplied explicitly. The frontend used for verification was version 1.22.1. Persistence checks also start `xdg-permission-store` from that existing frontend package, with a private data directory.

After building both repositories with their harness clients, run from this repository:

```sh
bash ../umbriel/tests/input_capture.sh ../umbriel/build-debug ./build-debug /path/to/xdg-desktop-portal
```

The check exercises v2 session creation, physical approval and denial, unauthorized cancellation, key and pointer capture, local event suppression, held-input cleanup, emergency release, session locking, output changes, disconnects, and fresh permission requests. It leaves the host compositor and session services unchanged.

## License

MIT License. See [LICENSE](LICENSE) for details.
