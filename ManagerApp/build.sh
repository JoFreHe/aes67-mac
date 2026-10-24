#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
BUILD_DIR="${BUILD_DIR:-../build}"
HELPERS=(AES67Discover AES67TestReceiver)
for HELPER in "${HELPERS[@]}"; do
  HELPER_PATH="$BUILD_DIR/Tools/$HELPER"
  if [ ! -x "$HELPER_PATH" ]; then
    echo "Missing required helper: $HELPER_PATH" >&2
    echo "Build the CMake targets AES67Discover and AES67TestReceiver first (or set BUILD_DIR)." >&2
    exit 1
  fi
done
mkdir -p .build
XCODE_DEVELOPER=/Applications/Xcode.app/Contents/Developer
if [ -z "${SWIFTC:-}" ]; then
  if [ -x "$XCODE_DEVELOPER/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc" ]; then
    SWIFTC="$XCODE_DEVELOPER/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc"
  else
    SWIFTC="$(xcrun -f swiftc)"
  fi
fi
if [ -z "${SDKROOT:-}" ]; then
  if [ -d "$XCODE_DEVELOPER/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" ]; then
    SDKROOT="$XCODE_DEVELOPER/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
  else
    SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
  fi
fi
SDK="$SDKROOT"
APP="AES67Manager.app"
STAGE_ROOT="$(mktemp -d "$PWD/.build/manager-build.XXXXXX")"
STAGED_APP="$STAGE_ROOT/$APP"
BACKUP_APP="$PWD/.build/AES67Manager.previous.$$"
OLD_APP_MOVED=0
APP_INSTALLED=0

cleanup() {
  local status=$?
  trap - EXIT
  if [ "$OLD_APP_MOVED" -eq 1 ] && [ "$APP_INSTALLED" -eq 0 ] && [ ! -e "$APP" ] && [ -e "$BACKUP_APP" ]; then
    if ! mv "$BACKUP_APP" "$APP"; then
      echo "Build failed and the previous app could not be restored from $BACKUP_APP" >&2
      status=1
    fi
  fi
  if [ "$APP_INSTALLED" -eq 1 ] && [ -e "$BACKUP_APP" ]; then
    rm -rf "$BACKUP_APP"
  fi
  rm -rf "$STAGE_ROOT"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$STAGED_APP/Contents/MacOS" "$STAGED_APP/Contents/Resources" "$STAGE_ROOT/ModuleCache"
for ARCH in x86_64 arm64; do
  "$SWIFTC" -O -file-prefix-map "$PWD=ManagerApp" -debug-prefix-map "$PWD=ManagerApp" -parse-as-library -target "$ARCH-apple-macos11.0" -sdk "$SDK" -module-cache-path "$STAGE_ROOT/ModuleCache" -framework AppKit -framework Foundation -framework SystemConfiguration ManagerMain.swift -o "$STAGE_ROOT/AES67Manager-$ARCH"
done
lipo -create "$STAGE_ROOT/AES67Manager-x86_64" "$STAGE_ROOT/AES67Manager-arm64" -output "$STAGED_APP/Contents/MacOS/AES67Manager"
cp Resources/Info.plist "$STAGED_APP/Contents/Info.plist"
VERSION_STRING="$(tr -d '[:space:]' < ../VERSION.txt)"
if [[ ! "$VERSION_STRING" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)-build\.([0-9]+)$ ]]; then
  echo "Invalid VERSION.txt value: $VERSION_STRING" >&2
  exit 1
fi
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}" "$STAGED_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${BASH_REMATCH[4]}" "$STAGED_APP/Contents/Info.plist"
./build-icon.sh "$STAGED_APP/Contents/Resources/AppIcon.icns"
for HELPER in "${HELPERS[@]}"; do
  cp "$BUILD_DIR/Tools/$HELPER" "$STAGED_APP/Contents/Resources/$HELPER"
  codesign --force --sign - "$STAGED_APP/Contents/Resources/$HELPER"
  codesign --verify --strict "$STAGED_APP/Contents/Resources/$HELPER"
done
codesign --force --deep --sign - "$STAGED_APP"
codesign --verify --strict --deep "$STAGED_APP"

if [ -e "$APP" ]; then
  if [ -e "$BACKUP_APP" ]; then
    echo "Refusing to replace existing backup: $BACKUP_APP" >&2
    exit 1
  fi
  mv "$APP" "$BACKUP_APP"
  OLD_APP_MOVED=1
fi
if ! mv "$STAGED_APP" "$APP"; then
  echo "Could not install the validated app bundle at $APP" >&2
  if [ "$OLD_APP_MOVED" -eq 1 ]; then
    if mv "$BACKUP_APP" "$APP"; then
      OLD_APP_MOVED=0
    else
      echo "Could not restore the previous app bundle from $BACKUP_APP" >&2
    fi
  fi
  exit 1
fi
APP_INSTALLED=1
echo "Built $APP (Universal2, macOS 11+)"
