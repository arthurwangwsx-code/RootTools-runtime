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

sign_macho() {
  local target="$1"
  if command -v ldid >/dev/null 2>&1; then
    ldid -S "$target"
    return
  fi
  if command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - --timestamp=none "$target"
    codesign --verify --strict "$target"
    return
  fi
  echo "No host ad-hoc signing tool is available for $target" >&2
  exit 1
}

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
sign_macho build/daemon/roottools-execd
sign_macho build/daemon/roottools-updater

xcodebuild -project RootTools.xcodeproj -scheme RootTools -configuration Release -sdk iphoneos \
  -derivedDataPath build/DerivedData CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build

APP="build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
sign_macho "$APP/RootTools"
echo "Built app: $APP"

