#!/bin/sh
# Run the native Linux reader against a neutral mount table. Only this
# process's mount namespace is changed. Exit 77 where user namespaces are disabled.
set -eu
if [ "${1-}" != --inside ]; then
    if ! unshare -Urnm true 2>/dev/null; then
        echo 'file places: mount namespace unavailable'
        exit 77
    fi
    exec unshare -Urnm sh "$0" --inside "$@"
fi
shift
fixture=$(mktemp)
trap 'rm -f "$fixture"' EXIT HUP INT TERM
cat > "$fixture" <<'MOUNTS'
/dev/demo / ext4 rw 0 0
/dev/demo /mnt/Music ext4 rw 0 0
/dev/usb /mnt/USB\040Drive vfat rw 0 0
server:/share /mnt/Shared nfs rw 0 0
proc /mnt/kernel proc rw 0 0
overlay /var/lib/containers overlay rw 0 0
/dev/demo /etc/hosts ext4 rw 0 0
MOUNTS
# /proc/self is resolved by mount's process; explicitly address the shell
# that exec will replace, so the reader sees the fixture at /proc/self/mounts.
mount --bind "$fixture" "/proc/$$/mounts"
export CUI_TEST_MOUNTS="$fixture" HOME=/home/demo
# The open bind mount keeps the file available; the test updates it through
# this process's proc entry, so the temporary directory entry can be removed.
rm -f "$fixture"
export CUI_TEST_MOUNTS=/proc/self/mounts
exec "$@"
