#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TOKEN_FILE="$ROOT/.roottools-token"
if [[ ! -f "$TOKEN_FILE" ]]; then openssl rand -hex 24 > "$TOKEN_FILE"; fi
TOKEN="$(cat "$TOKEN_FILE")"
AGENT_TOKEN_FILE="$ROOT/.roottools-agent-token"
if [[ ! -f "$AGENT_TOKEN_FILE" ]]; then openssl rand -hex 24 > "$AGENT_TOKEN_FILE"; fi
AGENT_TOKEN="$(cat "$AGENT_TOKEN_FILE")"

mkdir -p build/generated build/daemon Generated

# Keep generated build artifacts local. The script only creates build inputs
# and does not modify tracked source files.
cat > Generated/BuildToken.swift <<EOF
import Foundation

enum BuildToken {
    static let value = "$TOKEN"
}
EOF
sed -e "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" \
    -e "s/__ROOTTOOLS_AGENT_TOKEN__/$AGENT_TOKEN/g" \
    Daemon/roottools_execd.c > build/generated/roottools_execd.c
sed -e "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" \
    Daemon/roottools_updater.c > build/generated/roottools_updater.c

if command -v xcodegen >/dev/null 2>&1; then
    xcodegen generate
else
    echo "xcodegen not found, using existing RootTools.xcodeproj"
fi

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 \
  -I Daemon build/generated/roottools_execd.c Daemon/control_plane.c Daemon/provider_registry.c Daemon/package_controller.c Daemon/update_controller.c Daemon/runtime_observer.c -lsqlite3 -lz -framework CoreFoundation -o build/daemon/roottools-execd
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 \
  -I Daemon build/generated/roottools_updater.c Daemon/provider_registry.c Daemon/package_controller.c Daemon/update_controller.c -lsqlite3 -lz -framework CoreFoundation -o build/daemon/roottools-updater
if command -v ldid >/dev/null 2>&1; then
    ldid -S build/daemon/roottools-execd
    ldid -S build/daemon/roottools-updater
else
    echo "ldid not found, skipping daemon/updater ad-hoc signing"
fi

xcodebuild -project RootTools.xcodeproj -scheme RootTools -configuration Release -sdk iphoneos \
  -derivedDataPath build/DerivedData CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build

APP="build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
if command -v ldid >/dev/null 2>&1; then
    ldid -S "$APP/RootTools"
else
    echo "ldid not found, skipping app ad-hoc signing"
fi
echo "Built app: $APP"

