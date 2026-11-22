#!/bin/sh
# Integration check on a disposable desktop with both display servers running.
# Arguments: explicit-wayland and default-X11 builds' tests/cui_app_smoke.
set -eu
: "${WAYLAND_DISPLAY:?need a Wayland compositor}" "${DISPLAY:?need a working X server}"
wayland=$(realpath "$1"); x11=$(realpath "$2")
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cd "$work"
unset SDL_VIDEODRIVER SDL_VIDEO_DRIVER WAYLAND_SOCKET
export CUI_RENDERER=software
# Read what the client sends to the compositor, not what SDL says it chose.
env -u DISPLAY WAYLAND_DEBUG=client "$wayland" wayland.png 2>protocol.log
grep -E 'xdg_toplevel[^ ]*\.set_app_id\("io.github.yappologistic.CUI.SmokeTest"\)' protocol.log
# Unsetting WAYLAND_DISPLAY alone would still allow the default wayland-0.
export WAYLAND_DISPLAY="$work/no-wayland-socket"
"$x11" x11.png
status=0; "$wayland" unexpected.png >negative.log 2>&1 || status=$?
test "$status" = 1 && grep -Fx 'could not create app' negative.log || exit 1
printf '%s\n' 'Wayland build rejected X11-only display'
