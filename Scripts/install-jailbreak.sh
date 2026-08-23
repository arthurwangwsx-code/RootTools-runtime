#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
UDID="${ROOTTOOLS_UDID:-$(idevice_id -l | head -1)}"
[[ -n "$UDID" ]] || { echo "No USB iPhone found" >&2; exit 1; }

bash Scripts/build.sh
APP="build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
REMOTE_APP="/var/jb/Applications/RootTools.app"

python3 Scripts/root_exec.py --udid "$UDID" exec "rm -rf '$REMOTE_APP'; mkdir -p '$REMOTE_APP'"
while IFS= read -r -d '' file; do
  rel="${file#$APP/}"
  python3 Scripts/root_exec.py --udid "$UDID" push "$file" "$REMOTE_APP/$rel"
done < <(find "$APP" -type f -print0)
python3 Scripts/root_exec.py --udid "$UDID" exec "find '$REMOTE_APP' -type f -name '*.dylib' -exec chmod 755 {} \\; 2>/dev/null || true; chmod 755 '$REMOTE_APP/RootTools'"

python3 Scripts/root_exec.py --udid "$UDID" push build/daemon/roottools-execd /var/jb/usr/local/bin/roottools-execd
python3 Scripts/root_exec.py --udid "$UDID" push Daemon/com.arthur.roottools.execd.plist /var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist
python3 Scripts/root_exec.py --udid "$UDID" exec "chmod 755 /var/jb/usr/local/bin/roottools-execd; chown 0:0 /var/jb/usr/local/bin/roottools-execd /var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist; launchctl bootout system/com.arthur.roottools.execd >/dev/null 2>&1 || true; launchctl bootstrap system /var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist; sleep 1; /var/jb/usr/bin/uicache -p '$REMOTE_APP'"

echo "Installed RootTools + root daemon on $UDID"

