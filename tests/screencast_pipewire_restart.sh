#!/usr/bin/env bash
# Restarts a private PipeWire daemon under a running portal. New shares must
# deliver frames afterwards, and shares from before a restart must close cleanly.
set -euo pipefail

readonly BUILD=$(realpath "${1:?usage: screencast_pipewire_restart.sh UMBRIEL_BUILD PORTAL_BUILD}")
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
for command in dbus-daemon dbus-send pipewire pw-link; do
  command -v "$command" >/dev/null || {
    echo "Missing test command: $command"
    exit 2
  }
done
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

cat >"$RUNTIME/config.toml" <<'CONFIG'
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
[output.HEADLESS-1]
mode = "640x360"
scale = 1
hdr = "off"
[[window_rule]]
match.title = "^screencast-restart$"
default_floating = true
default_floating_size_px = { width = 640, height = 360 }
default_position = { x = 0, y = 0, anchor = "top_left" }
CONFIG
# The fake inherited display also prevents global activation-environment updates.
WAYLAND_DISPLAY=screencast-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  "$UMBRIEL" -c "$RUNTIME/config.toml" >"$RUNTIME/compositor.log" 2>&1 &
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

# Every share captures the whole output, which this window fills with red.
FILL_COLOR=0xFFFF0000 "$BUILD/tests/unmap-client" screencast-restart 640 360 >"$RUNTIME/window.log" 2>&1 &
PIDS+=("$!")
wait_log "$RUNTIME/window.log" mapped
"$UMBRIEL" settle >/dev/null

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
for _ in $(seq 200); do
  [[ -S $RUNTIME/bus ]] && break
  sleep 0.025
done

start_pipewire() {
  pipewire >>"$RUNTIME/pipewire.log" 2>&1 &
  PIPEWIRE_PID=$!
  PIDS+=("$PIPEWIRE_PID")
  for _ in $(seq 200); do
    [[ -S $RUNTIME/pipewire-0 ]] && return 0
    sleep 0.025
  done
  echo 'Timed out waiting for PipeWire'
  return 1
}
restart_pipewire() {
  kill "$PIPEWIRE_PID"
  wait "$PIPEWIRE_PID" || true
  start_pipewire
}
require_portal() {
  kill -0 "$PORTAL_PID" 2>/dev/null || {
    echo 'Portal exited'
    cat "$RUNTIME/portal.log"
    exit 1
  }
}

# The portal starts before PipeWire is available.
"$PORTAL" >"$RUNTIME/portal.log" 2>&1 &
PORTAL_PID=$!
PIDS+=("$PORTAL_PID")
for _ in $(seq 200); do
  if dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.NameHasOwner string:org.freedesktop.impl.portal.desktop.umbriel 2>/dev/null | grep -q 'boolean true'; then break; fi
  sleep 0.025
done

if "$CLIENT" </dev/null >"$RUNTIME/client-early.log" 2>&1; then
  echo 'A share started without PipeWire'
  exit 1
fi
grep -q 'Start failed' "$RUNTIME/client-early.log" || {
  cat "$RUNTIME/client-early.log"
  exit 1
}
require_portal

declare -A SHARE_FD SHARE_PID
# Starts a share and waits for a red frame from it.
share() {
  local name=$1 log=$RUNTIME/client-$1.log fd node
  mkfifo "$RUNTIME/$name.fifo"
  exec {fd}<>"$RUNTIME/$name.fifo"
  "$CLIENT" <&"$fd" >"$log" 2>&1 &
  SHARE_FD[$name]=$fd
  SHARE_PID[$name]=$!
  PIDS+=("$!")
  wait_log "$log" 'ready node='
  node=$(sed -n 's/^ready node=//p' "$log" | head -1)
  ((node != 0)) || {
    echo "Share $name received no PipeWire node"
    exit 1
  }
  for _ in $(seq 200); do
    if pw-link -i | grep -q 'umbriel-screencast-test:input_1'; then break; fi
    sleep 0.025
  done
  pw-link 'umbriel-screen-capture:capture_1' 'umbriel-screencast-test:input_1'
  printf 'expect %s 640 360 ff0000 ff0000 ff0000 ff0000\n' "$name" >&"$fd"
  wait_log "$log" "matched label=$name node=$node "
}
# Closes a share whose PipeWire daemon has since been replaced.
close_old_share() {
  local name=$1
  kill -0 "${SHARE_PID[$name]}" || {
    echo "Share $name exited before it was closed"
    exit 1
  }
  printf 'quit\n' >&"${SHARE_FD[$name]}"
  wait "${SHARE_PID[$name]}" || true
  require_portal
}

start_pipewire
share first
restart_pipewire
share second
close_old_share first
restart_pipewire
share third
close_old_share second
printf 'quit\n' >&"${SHARE_FD[third]}"
wait "${SHARE_PID[third]}"
require_portal
SUCCESS=1
echo 'ScreenCast recovered from a late PipeWire start and two restarts, and closed old shares cleanly'
