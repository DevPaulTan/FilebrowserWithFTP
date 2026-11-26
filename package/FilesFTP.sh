#!/bin/sh
# Launcher for "Files + FTP" (shows up in App Center).
# The app lives in the FilesFTP folder next to this script.
DIR="$(cd "$(dirname "$0")" && pwd)/FilesFTP"
cd "$DIR" || exit 1
chmod +x ./rg35xx-files 2>/dev/null
./rg35xx-files > "$DIR/log.txt" 2>&1
sync
