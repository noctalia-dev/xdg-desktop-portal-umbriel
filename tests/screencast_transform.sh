#!/usr/bin/env bash
# Receives real PipeWire frames from a private headless compositor and portal.
# Every phase changes the scene so an old frame cannot satisfy a new expectation.
set -euo pipefail

readonly BUILD=$(realpath "${1:?usage: screencast_transform.sh UMBRIEL_BUILD PORTAL_BUILD}")
readonly PORTAL_BUILD=$(realpath "${2:?portal build directory required}")
readonly UMBRIEL=$BUILD/umbriel
readonly PORTAL=${UMBRIEL_SCREENCAST_PORTAL:-$PORTAL_BUILD/xdg-desktop-portal-umbriel}
readonly CLIENT=$PORTAL_BUILD/screencast-test-client
readonly RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/umbriel-screencast.XXXXXXXX")
chmod 700 "$RUNTIME"
PIDS=()
SUCCESS=0
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  if ((SUCCESS)) && [[ ${UMBRIEL_SCREENCAST_KEEP_LOGS:-0} != 1 ]]; then
    rm -rf "$RUNTIME"
  else
    echo "ScreenCast logs retained in $RUNTIME"
  fi
}
trap cleanup EXIT

for executable in "$UMBRIEL" "$PORTAL" "$CLIENT" "$BUILD/tests/unmap-client"; do
  [[ -x $executable ]] || {
    echo "Missing executable: $executable"
    exit 2
  }
done
for command in dbus-daemon dbus-send pipewire pw-link jq; do
  command -v "$command" >/dev/null || {
    echo "Missing test command: $command"
    exit 2
  }
done
"$UMBRIEL" --help | grep -qw settle || {
  echo 'Umbriel needs -Dtest_ipc=enabled'
  exit 2
}
export XDG_RUNTIME_DIR=$RUNTIME
export XDG_CONFIG_HOME=$RUNTIME/config
export XDG_CACHE_HOME=$RUNTIME/cache
export XDG_DATA_HOME=$RUNTIME/data
export XDG_STATE_HOME=$RUNTIME/state
export XDG_CONFIG_DIRS=$RUNTIME/config-dirs
export XDG_DATA_DIRS=$RUNTIME/data-dirs
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/no-system-bus
export PIPEWIRE_RUNTIME_DIR=$RUNTIME
export PIPEWIRE_REMOTE=pipewire-0
export XDG_SESSION_TYPE=wayland
unset DISPLAY WAYLAND_SOCKET NOTIFY_SOCKET SYSTEMD_EXEC_PID PIPEWIRE_CONFIG_DIR PIPEWIRE_CONFIG_NAME
mkdir -p "$XDG_CONFIG_HOME/xdg-desktop-portal-umbriel" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME"

readonly CONFIG=$RUNTIME/config.toml
cat >"$RUNTIME/base.toml" <<'CONFIG'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[screencast]
disable_dynamic_confirmation = true
[[window_rule]]
match.title = "^screencast-transform-[0-3]$"
default_floating = true
default_floating_size_px = { width = 80, height = 80 }
default_position = { x = 0, y = 0, anchor = "top_left" }
CONFIG
write_config() {
  cat "$RUNTIME/base.toml" >"$CONFIG"
  cat >>"$CONFIG" <<CONFIG
[output.HEADLESS-1]
mode = "640x360"
scale = 1
hdr = "off"
transform = "$1"
CONFIG
}
write_config normal
# The fake inherited display also prevents global activation-environment updates.
WAYLAND_DISPLAY=screencast-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  "$UMBRIEL" -c "$CONFIG" >"$RUNTIME/compositor.log" 2>&1 &
PIDS+=("$!")
export WAYLAND_DISPLAY=wayland-0
export UMBRIEL_SOCKET=$RUNTIME/umbriel-wayland-0.sock
for _ in $(seq 200); do
  [[ -S $UMBRIEL_SOCKET ]] && break
  sleep 0.025
done
[[ -S $UMBRIEL_SOCKET ]] || {
  cat "$RUNTIME/compositor.log"
  exit 1
}

wait_log() {
  local log=$1 text=$2
  for _ in $(seq 240); do
    grep -Fq "$text" "$log" 2>/dev/null && return 0
    sleep 0.025
  done
  echo "Timed out waiting for '$text' in $log"
  cat "$log"
  return 1
}

WINDOW_IDS=()
WINDOW_FDS=()
colors=(0xFFFF0000 0xFF00FF00 0xFF0000FF 0xFFFFFFFF)
for index in 0 1 2 3; do
  mkfifo "$RUNTIME/window-$index.fifo"
  exec {window_fd}<>"$RUNTIME/window-$index.fifo"
  WINDOW_FDS+=("$window_fd")
  FILL_COLOR=${colors[index]} REPAINT_ON_STDIN=1 \
    "$BUILD/tests/unmap-client" "screencast-transform-$index" 80 80 <&"$window_fd" >"$RUNTIME/window-$index.log" 2>&1 &
  PIDS+=("$!")
  wait_log "$RUNTIME/window-$index.log" mapped
  id=$("$UMBRIEL" windows --json | jq -er --arg title "screencast-transform-$index" '.[] | select(.title == $title) | .id')
  WINDOW_IDS+=("$id")
done

WIDTH=640
HEIGHT=360
configure() {
  write_config "$1"
  "$UMBRIEL" msg config-reload >/dev/null
  "$UMBRIEL" settle >/dev/null
  case $1 in
    90 | 270 | flipped-90 | flipped-270)
      WIDTH=360
      HEIGHT=640
      ;;
    *)
      WIDTH=640
      HEIGHT=360
      ;;
  esac
  local index x y
  for index in 0 1 2 3; do
    x=$(((1 + 2 * (index % 2)) * WIDTH / 4 - 40))
    y=$(((1 + 2 * (index / 2)) * HEIGHT / 4 - 40))
    "$UMBRIEL" msg "window-move-to:top_left/$x/$y/${WINDOW_IDS[index]}" >/dev/null
  done
  "$UMBRIEL" settle >/dev/null
}
configure normal

cat >"$RUNTIME/choose" <<'CHOOSER'
#!/usr/bin/env bash
cat >/dev/null
printf '%s\n' '{"selections":[{"kind":"monitor","output":"HEADLESS-1"}]}'
CHOOSER
chmod +x "$RUNTIME/choose"
cat >"$XDG_CONFIG_HOME/xdg-desktop-portal-umbriel/config.toml" <<CONFIG
[screencast]
chooser_cmd = "$RUNTIME/choose"
max_fps = 30
CONFIG

dbus-daemon --session --nofork --address="$DBUS_SESSION_BUS_ADDRESS" >"$RUNTIME/bus.log" 2>&1 &
PIDS+=("$!")
pipewire >"$RUNTIME/pipewire.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 200); do
  [[ -S $RUNTIME/bus && -S $RUNTIME/pipewire-0 ]] && break
  sleep 0.025
done
"$PORTAL" >"$RUNTIME/portal.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 200); do
  if dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.NameHasOwner string:org.freedesktop.impl.portal.desktop.umbriel 2>/dev/null | grep -q 'boolean true'; then break; fi
  sleep 0.025
done
mkfifo "$RUNTIME/client.fifo"
exec {CLIENT_FD}<>"$RUNTIME/client.fifo"
"$CLIENT" <&"$CLIENT_FD" >"$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
PIDS+=("$CLIENT_PID")
wait_log "$RUNTIME/client.log" 'ready node='
readonly NODE=$(sed -n 's/^ready node=//p' "$RUNTIME/client.log" | head -1)
for _ in $(seq 200); do
  if pw-link -i | grep -q 'umbriel-screencast-test:input_1'; then break; fi
  sleep 0.025
done
pw-link 'umbriel-screen-capture:capture_1' 'umbriel-screencast-test:input_1'

PHASE=0
LEFT=ff0000
RIGHT=00ff00
arm() {
  printf 'expect %s %s %s %s %s %s %s\n' "$@" >&"$CLIENT_FD"
  wait_log "$RUNTIME/client.log" "expecting label=$1 "
}
# Swapping only the upper two colors gives a fresh signature which is not a
# rotation or reflection of the previous frame: blue and white stay in place.
next_palette() {
  PHASE=$((PHASE + 1))
  if ((PHASE % 2)); then
    LEFT=00ff00
    RIGHT=ff0000
  else
    LEFT=ff0000
    RIGHT=00ff00
  fi
}
repaint() {
  if ((PHASE % 2)); then
    printf G >&"${WINDOW_FDS[0]}"
    printf R >&"${WINDOW_FDS[1]}"
  else
    printf R >&"${WINDOW_FDS[0]}"
    printf G >&"${WINDOW_FDS[1]}"
  fi
}
matched() {
  wait_log "$RUNTIME/client.log" "matched label=$1 node=$NODE "
}

# Keep the same portal session, PipeWire source node and consumer connection
# through a round trip and then every remaining rotation and reflection.
for transform in normal 180 90 normal 270 flipped flipped-90 flipped-180 flipped-270; do
  configure "$transform"
  next_palette
  label="output-$PHASE-$transform"
  arm "$label" "$WIDTH" "$HEIGHT" "$LEFT" "$RIGHT" 0000ff ffffff
  repaint
  matched "$label"
done

# Changing targets must also keep the same PipeWire node and negotiate the
# window's own size before returning to a rotated output's displayed dimensions.
configure 180
next_palette
arm window 80 80 "$LEFT" "$LEFT" "$LEFT" "$LEFT"
"$UMBRIEL" msg "screencast-set-window:${WINDOW_IDS[0]}" >/dev/null
repaint
matched window
configure 90
next_palette
arm return-output "$WIDTH" "$HEIGHT" "$LEFT" "$RIGHT" 0000ff ffffff
"$UMBRIEL" msg screencast-set-output:HEADLESS-1 >/dev/null
repaint
matched return-output

printf 'quit\n' >&"$CLIENT_FD"
wait "$CLIENT_PID"
SUCCESS=1
echo 'ScreenCast pixels, dimensions, live transforms and output/window switches verified on one PipeWire node'
