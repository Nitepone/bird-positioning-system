#!/bin/sh
# Runs a command in /firmware as root (the ESP32 platform rewrites its own
# package files on every build, which only their owner may do), then gives
# whatever it created - build output, config files - to the owner of the
# mounted project, so nothing in the checkout is left owned by root.
"$@"
status=$?
owner=$(stat -c %u:%g /firmware)
if [ "$owner" != "0:0" ]; then
    find /firmware -xdev -user 0 -exec chown -h "$owner" {} + 2>/dev/null
fi
exit $status
