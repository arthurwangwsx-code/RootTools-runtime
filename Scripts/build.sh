#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TOKEN_FILE="$ROOT/.roottools-token"
if [[ ! -f "$TOKEN_FILE" ]]; then openssl rand -hex 24 > "$TOKEN_FILE"; fi
TOKEN="$(cat "$TOKEN_FILE")"

mkdir -p build/generated build/daemon Generated
cat > Generated/BuildToken.swift <<EOF
import Foundation

enum BuildToken {
    static let value = "$TOKEN"
}
EOF
sed "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" Daemon/roottools_execd.c > build/generated/roottools_execd.c

xcodegen generate

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 \
  build/generated/roottools_execd.c -o build/daemon/roottools-execd
ldid -S build/daemon/roottools-execd

xcodebuild -project RootTools.xcodeproj -scheme RootTools -configuration Release -sdk iphoneos \
  -derivedDataPath build/DerivedData CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build

APP="build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
ldid -S "$APP/RootTools"
echo "$APP"

