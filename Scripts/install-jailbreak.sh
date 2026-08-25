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
python3 Scripts/root_exec.py --udid "$UDID" push build/daemon/roottools-updater /var/jb/usr/local/bin/roottools-updater
python3 Scripts/root_exec.py --udid "$UDID" push Daemon/com.arthur.roottools.execd.plist /var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist
python3 Scripts/root_exec.py --udid "$UDID" push Daemon/com.arthur.roottools.updater.plist /var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist

# A fresh development host may not have ldid installed. In that case sign the
# final device-side binaries with the jailbreak bootstrap's ldid before launchd
# or SpringBoard attempts to execute them. Fail closed if neither side can sign.
if ! command -v ldid >/dev/null 2>&1; then
  python3 Scripts/root_exec.py --udid "$UDID" exec "LDID=/var/jb/usr/bin/ldid; [ -x \"\$LDID\" ] || { echo 'device ldid is unavailable' >&2; exit 42; }; \"\$LDID\" -S '$REMOTE_APP/RootTools'; \"\$LDID\" -S /var/jb/usr/local/bin/roottools-execd; \"\$LDID\" -S /var/jb/usr/local/bin/roottools-updater"
fi

python3 Scripts/root_exec.py --udid "$UDID" exec "chmod 755 /var/jb/usr/local/bin/roottools-execd /var/jb/usr/local/bin/roottools-updater; chown 0:0 /var/jb/usr/local/bin/roottools-execd /var/jb/usr/local/bin/roottools-updater /var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist /var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist; launchctl bootout system/com.arthur.roottools.execd >/dev/null 2>&1 || true; launchctl bootout system/com.arthur.roottools.updater >/dev/null 2>&1 || true; launchctl bootstrap system /var/jb/Library/LaunchDaemons/com.arthur.roottools.execd.plist; launchctl bootstrap system /var/jb/Library/LaunchDaemons/com.arthur.roottools.updater.plist; sleep 1; /var/jb/usr/bin/uicache -p '$REMOTE_APP'"

echo "Installed RootTools + root daemon on $UDID"

