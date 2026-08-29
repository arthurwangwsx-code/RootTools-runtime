#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source "$ROOT/Scripts/credential-files.sh"
source "$ROOT/Scripts/version.sh"

TOKEN_FILE="$ROOT/.roottools-token"
AGENT_TOKEN_FILE="$ROOT/.roottools-agent-token"
TOKEN="$(roottools_read_or_create_token "$TOKEN_FILE" "Owner token")"
AGENT_TOKEN="$(roottools_read_or_create_token "$AGENT_TOKEN_FILE" "Agent token")"
PACKAGE_VERSION="$(roottools_package_version "$ROOT/VERSION")"
DAEMON_VERSION="$(roottools_daemon_version "$PACKAGE_VERSION")"

mkdir -p build/generated build/daemon build/obj/ios Generated

sign_macho() {
  local target="$1"
  # On macOS prefer the platform signer. Its ad-hoc CodeDirectory can be
  # verified before packaging, while device-side ldid may still refresh the
  # signature during postinst. A host ldid installation should not silently
  # downgrade build-time verification quality.
  if [[ "$(uname -s)" == "Darwin" ]] && command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - --timestamp=none "$target"
    codesign --verify --strict "$target"
    return
  fi
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

# Keep generated build artifacts local. Preserve mtimes when content is
# unchanged so Xcode incremental builds do not recompile the entire SwiftUI
# target merely because build.sh was invoked again.
write_if_changed() {
  local target="$1"
  local temp="$target.tmp.$$"
  cat > "$temp"
  if [[ -f "$target" ]] && cmp -s "$temp" "$target"; then
    rm -f "$temp"
  else
    mv "$temp" "$target"
  fi
}

write_if_changed Generated/BuildToken.swift <<EOF
import Foundation

enum BuildToken {
    static let value = "$TOKEN"
}
EOF
printf '%s\n' "$PACKAGE_VERSION" | write_if_changed build/generated/package-version
printf '%s\n' "$DAEMON_VERSION" | write_if_changed build/generated/daemon-version
sed -e "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" \
    -e "s/__ROOTTOOLS_AGENT_TOKEN__/$AGENT_TOKEN/g" \
    -e "s/__ROOTTOOLS_VERSION__/$DAEMON_VERSION/g" \
    Daemon/roottools_execd.c > build/generated/roottools_execd.c.tmp
if [[ ! -f build/generated/roottools_execd.c ]] || ! cmp -s build/generated/roottools_execd.c.tmp build/generated/roottools_execd.c; then
  mv build/generated/roottools_execd.c.tmp build/generated/roottools_execd.c
else
  rm -f build/generated/roottools_execd.c.tmp
fi
sed -e "s/__ROOTTOOLS_TOKEN__/$TOKEN/g" \
    Daemon/roottools_updater.c > build/generated/roottools_updater.c.tmp
if [[ ! -f build/generated/roottools_updater.c ]] || ! cmp -s build/generated/roottools_updater.c.tmp build/generated/roottools_updater.c; then
  mv build/generated/roottools_updater.c.tmp build/generated/roottools_updater.c
else
  rm -f build/generated/roottools_updater.c.tmp
fi

if command -v xcodegen >/dev/null 2>&1; then
    xcodegen generate
else
    echo "xcodegen not found, using existing RootTools.xcodeproj"
fi

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
IOS_CLANG=(xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -I Daemon)

compile_ios_object() {
  local source="$1"
  local object="$2"
  local rebuild=0
  if [[ ! -f "$object" || "$source" -nt "$object" ]]; then
    rebuild=1
  else
    local header
    for header in Daemon/*.h; do
      if [[ "$header" -nt "$object" ]]; then rebuild=1; break; fi
    done
  fi
  if [[ "$rebuild" -eq 1 ]]; then
    "${IOS_CLANG[@]}" -c "$source" -o "$object"
  fi
}

DAEMON_SOURCES=(
  build/generated/roottools_execd.c
  Daemon/control_plane.c
  Daemon/provider_registry.c
  Daemon/package_controller.c
  Daemon/update_controller.c
  Daemon/runtime_observer.c
  Daemon/remote_worker_controller.c
  Daemon/remote_access_controller.c
  Daemon/principal_store.c
)
DAEMON_OBJECTS=()
pids=()
for source in "${DAEMON_SOURCES[@]}"; do
  base="$(basename "$source" .c)"
  object="build/obj/ios/daemon-${base}.o"
  DAEMON_OBJECTS+=("$object")
  compile_ios_object "$source" "$object" &
  pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done

UPDATER_SOURCES=(
  build/generated/roottools_updater.c
  Daemon/provider_registry.c
  Daemon/package_controller.c
  Daemon/update_controller.c
)
UPDATER_OBJECTS=()
pids=()
for source in "${UPDATER_SOURCES[@]}"; do
  base="$(basename "$source" .c)"
  object="build/obj/ios/updater-${base}.o"
  UPDATER_OBJECTS+=("$object")
  compile_ios_object "$source" "$object" &
  pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done

link_if_needed() {
  local output="$1"
  shift
  local needs_link=0
  if [[ ! -f "$output" ]]; then
    needs_link=1
  else
    local object
    for object in "$@"; do
      if [[ "$object" -nt "$output" ]]; then needs_link=1; break; fi
    done
  fi
  if [[ "$needs_link" -eq 1 ]]; then
    "${IOS_CLANG[@]}" "$@" -lsqlite3 -lz -framework CoreFoundation -o "$output"
    sign_macho "$output"
  fi
}

link_if_needed build/daemon/roottools-execd "${DAEMON_OBJECTS[@]}"
link_if_needed build/daemon/roottools-updater "${UPDATER_OBJECTS[@]}"
# Signing is cheap and part of the artifact contract. Re-verify even when the
# linker was skipped due to a warm object cache.
sign_macho build/daemon/roottools-execd
sign_macho build/daemon/roottools-updater

# Keep the scheme so `-destination` is honored, but pin it to the generic iOS
# device. Without the explicit generic destination Xcode may prepare debugger
# support for whatever physical iPhone happens to be attached before it even
# reaches the compiler.
xcodebuild -project RootTools.xcodeproj -scheme RootTools -configuration Release -sdk iphoneos \
  -destination 'generic/platform=iOS' -derivedDataPath build/DerivedData \
  MARKETING_VERSION="$DAEMON_VERSION" CURRENT_PROJECT_VERSION="${PACKAGE_VERSION##*-}" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build

APP="build/DerivedData/Build/Products/Release-iphoneos/RootTools.app"
sign_macho "$APP/RootTools"
echo "Built app: $APP"
