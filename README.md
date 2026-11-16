# xdg-desktop-portal-umbriel

An [xdg-desktop-portal](https://github.com/flatpak/xdg-desktop-portal) backend for the [Umbriel](https://github.com/noctalia-dev/umbriel) compositor.

## Supported interfaces

- `org.freedesktop.impl.portal.GlobalShortcuts` (version 1)
- `org.freedesktop.impl.portal.InputCapture` (version 2)
- `org.freedesktop.impl.portal.ScreenCast`
- `org.freedesktop.impl.portal.Screenshot`

## Global shortcuts

The GlobalShortcuts backend implements version 1: keyboard shortcut registration, listing, activation and release signals, and notifications when bindings change. Applications use [org.freedesktop.portal.GlobalShortcuts](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html) through `xdg-desktop-portal`. Bindings are edited in Umbriel's TOML configuration; the version 2 `ConfigureShortcuts` method and an interactive shortcut editor are unsupported.

Requires an Umbriel build providing version 1 of `umbriel_global_shortcuts_manager_v1`. This private bridge version is independent of the D-Bus interface version. On compositor builds without this bridge, GlobalShortcuts requests fail while the other portal interfaces remain available.

Umbriel asks for physical approval before registering new shortcut IDs. Review the application label, shortcut IDs, descriptions, and candidate chords; the application label is unverified. Press Enter on a physical keyboard to advance through review pages, then press Enter again on the final page to approve and remember the request. Another non-modifier physical key or a pointer click cancels. Virtual keyboards and emulated input cannot approve. Each session allows one bind attempt, so retry with a new session after cancellation or failure.

Set overrides in **Umbriel's configuration file**:

```toml
[global_shortcuts."org.example.App"]
"push-to-talk" = "Mod+Shift+M"
"toggle-overlay" = ""
```

Use the exact application and shortcut IDs from `umbriel global-shortcuts --json`. An empty value disables a shortcut. Overrides take priority over remembered and application-preferred triggers; removing an override restores the remembered preference. New shortcut IDs still require approval. Plain or Shift-only printable preferred triggers are left unassigned; explicit overrides can assign them. Mouse buttons, wheel events, modifier-only chords, and multi-chord sequences are unsupported.

Configured keybinds in the default submap, Ctrl+Alt+Escape, and VT-switch chords remain reserved. Other live shortcut conflicts follow registration order. Disabled, unassigned, and conflicting shortcuts are omitted from bound results. Shortcuts respect the focused application's shortcut inhibition and pause during input capture, locking, confirmation panels, active submaps, and session inactivity. Configuration reloads update affected bindings and stop held activations whose binding changed.

Approval records and preferences belong to Umbriel, in `$XDG_STATE_HOME/umbriel/global-shortcuts.json`. The backend keeps only opaque credentials keyed by application in `$XDG_STATE_HOME/xdg-desktop-portal-umbriel/global-shortcuts-credentials.json`, using an owned 0700 directory and atomic 0600 file updates. Both paths default under `~/.local/state`. Credentials stay out of portal replies, IPC listings, and logs. Missing or revoked credentials require fresh approval; failure to save a replacement fails the request and closes its new registration.

Inspect shortcuts and revoke remembered approval with:

```sh
umbriel global-shortcuts --json
umbriel global-shortcuts-revoke org.example.App
umbriel global-shortcuts-revoke all
```

Revocation closes matching live sessions and cancels pending review. Later requests require fresh approval. TOML overrides remain in place.

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

Tests, lint, and formatting commands are in [CONTRIBUTING.md](CONTRIBUTING.md).

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

A share with exactly one selected screen or window can be made changeable by enabling **Allow changing the source with Umbriel actions** in the share picker. Other shares are fixed to the source the user picked, so several shares from one application, such as separate OBS sources, each keep their own screen or window and are not moved by Umbriel actions. The selected source starts sharing immediately. Shares containing several selected sources are always fixed because one target action cannot unambiguously identify which stream to replace.

All changeable shares follow one Umbriel target. A custom `chooser_cmd` opts in by adding `"dynamic": true` next to `"selections"` in its output; without it the share is fixed.

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

Selecting another target replaces only the Wayland capture source. The PipeWire node remains the same, so applications keep the original share without opening another chooser. Window and output targets are interchangeable on that stream. Applications can restore the selected source without reopening the chooser; restore data records whether the share was changeable, and restore data written before this choice existed restores a fixed share. A new session from the same application started within two seconds of a changeable share being granted or retargeted reuses its mode and target, including when that short-lived request closes during handoff. The grant is invalidated when the target changes or clears. Any other request, including a second concurrent share from the same application, opens the picker.

## InputCapture verification

The receiver helper is built when Meson's `tests` option is enabled. The complete integration check lives in the Umbriel repository and uses a private D-Bus session, an isolated headless compositor, and backend-boundary input devices. It requires `dbus-daemon` and `dbus-send` from the `dbus` package, plus an `xdg-desktop-portal` frontend executable; the frontend path is supplied explicitly. The frontend used for verification was version 1.22.1. Persistence checks also start `xdg-permission-store` from that existing frontend package, with a private data directory.

After building both repositories with their harness clients, run from this repository:

```sh
bash ../umbriel/tests/input_capture.sh ../umbriel/build-debug ./build-debug /path/to/xdg-desktop-portal
```

The check exercises v2 session creation, physical approval and denial, unauthorized cancellation, key and pointer capture, local event suppression, held-input cleanup, emergency release, session locking, output changes, disconnects, and fresh permission requests. It leaves the host compositor and session services unchanged.

## License

MIT License. See [LICENSE](LICENSE) for details.
